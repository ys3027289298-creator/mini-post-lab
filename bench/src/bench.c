/*
 * bench: post-process output strategy comparison.
 *
 * 复现两条输出路径的代价结构：
 *   sep   上一版：3 个独立通道，各读所需输入、按原生格式写各自缓冲
 *   mrt   这一版：1 个多目标通道，读全部输入，3 个目标强制统一 RGBA16F
 *   split 修复版：拆回两张——pass1 打包 color+coc 写 RGBA8，pass2 bloom 写 RGBA16F
 *
 * 固定开销：每通道一次线程池派发。两种屏障可切换：
 *   pthread（系统态往返，重）与 spin（用户态 sense-reversing，轻，模拟 GPU 调度粒度）。
 * 带宽开销：按真实格式字节数分配缓冲并逐字节写入，流量是实际访存而非估算。
 * 计算：复用本仓库 tonemap(ACES)/dof(CoC)/bloom(bright-pass) 的标量公式。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

/* 15 worker + main = 16，占满本机 16 个逻辑核，自旋不超卖 */
#define NW 15
#define SYNC_PTHREAD 0
#define SYNC_SPIN    1
static int g_sync = SYNC_PTHREAD;

/* ---- 每像素格式字节数（带宽由此真实产生） ---- */
#define B_HDR   16  /* RGBA16F 场景输入        */
#define B_DEPTH  4  /* R32F 深度输入           */
#define B_LUM    2  /* R16F 亮度输入           */
#define B_COLOR  4  /* RGBA8  LDR 颜色（原生） */
#define B_COC    2  /* R16F   弥散圆（原生）   */
#define B_BLOOM  8  /* RGBA16F HDR 辉光（原生）*/
#define B_HI     8  /* RGBA16F MRT 统一高精度  */

/*
 * 通道枚举，每个通道只触碰自己激活的槽位，不存在空指针：
 *   P_TONEMAP_SEP  仅 color（RGBA8）
 *   P_DOF_SEP      仅 CoC（R16F）
 *   P_BLOOM        仅 bloom（RGBA16F）
 *   P_PACK         color+CoC 打包进 RGBA8 的 rgb+a
 *   P_MRT          一次写三个目标，统一 RGBA16F
 */
enum { P_TONEMAP_SEP, P_DOF_SEP, P_BLOOM, P_PACK, P_MRT };

typedef struct {
    int w, h, n;
    uint8_t *hdr;    /* HDR 输入：RGBA16F */
    uint8_t *depth;  /* R32F */
    uint8_t *lum;    /* R16F */
    uint8_t *o_color, *o_coc, *o_bloom; /* 输出缓冲，按模式取不同格式尺寸 */
} Frame;

/* ---- 线程池与两种屏障 ---- */
static pthread_t th[NW];
static pthread_barrier_t bar;
static atomic_int        acount = 0;
static atomic_int        asense = 0;
static _Thread_local int my_sense = 0;
#define PARTIES (NW + 1)

static void barrier_wait(void) {
    if (g_sync == SYNC_PTHREAD) { pthread_barrier_wait(&bar); return; }
    /* sense-reversing：最后到达者清零计数并翻转相位，其余人等本地相位出现 */
    my_sense ^= 1;
    int n = atomic_fetch_add_explicit(&acount, 1, memory_order_acq_rel) + 1;
    if (n == PARTIES) {
        atomic_store_explicit(&acount, 0, memory_order_relaxed);
        atomic_store_explicit(&asense, my_sense, memory_order_release);
    } else {
        while (atomic_load_explicit(&asense, memory_order_acquire) != my_sense)
            __asm__ __volatile__("pause" ::: "memory");
    }
}
static void barrier_init(void) { pthread_barrier_init(&bar, NULL, PARTIES); }

static volatile int    cur_pass;
static volatile int    shutdown_flag;
static const uint8_t  *g_src[3];
static uint8_t        *g_dst[3];
static int             g_lo, g_hi;
static uint64_t        g_check[NW];

static uint16_t f32_to_f16(float f) {
    union { float f; uint32_t u; } v = { f };
    uint32_t u = v.u;
    int sign = (u >> 16) & 0x8000;
    int exp  = ((u >> 23) & 0xff) - 127 + 15;
    uint32_t man = u & 0x7fffff;
    if (((u >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00 | (man ? 1 : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    if (exp <= 0)  return (uint16_t)sign;
    return (uint16_t)(sign | (exp << 10) | (man >> 13));
}
static float f16_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)(h & 0x8000)) << 16;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t man  = h & 0x3ff;
    uint32_t u;
    if (exp == 0) {
        if (man == 0) { u = sign; }
        else { exp = 127 - 15 + 1; while (!(man & 0x400)) { man <<= 1; exp--; } man &= 0x3ff;
               u = sign | (exp << 23) | (man << 13); }
    } else if (exp == 31) u = sign | 0x7f800000 | (man << 13);
    else u = sign | ((exp + (127 - 15)) << 23) | (man << 13);
    union { uint32_t u; float f; } v = { u };
    return v.f;
}

static float aces(float x) { /* 与 tonemap/src/tonemap.c 同式 */
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (x * (a * x + b)) / (x * (c * x + d) + e);
    return r < 0 ? 0 : (r > 1 ? 1 : r);
}
static float coc_of(float depth, float focus, float aperture, float focal) { /* 同 dof.c */
    float num = aperture * focal * (depth - focus);
    float den = depth * (focus - focal);
    return fabsf(num / den);
}

/* P_TONEMAP_SEP / P_PACK 共用：RGBA8，打包时 CoC 进 alpha */
static void tonemap_job(int lo, int hi, uint64_t *ckp) {
    uint64_t ck = 0;
    const uint8_t *s = g_src[0]; uint8_t *d = g_dst[0];
    int pack = (cur_pass == P_PACK);
    for (int i = lo; i < hi; i++) {
        float r = f16_to_f32((uint16_t)(s[i*8+0] | (s[i*8+1]<<8)));
        float g = f16_to_f32((uint16_t)(s[i*8+2] | (s[i*8+3]<<8)));
        float b = f16_to_f32((uint16_t)(s[i*8+4] | (s[i*8+5]<<8)));
        r = aces(r); g = aces(g); b = aces(b);
        d[i*4+0] = (uint8_t)(r*255); d[i*4+1] = (uint8_t)(g*255);
        d[i*4+2] = (uint8_t)(b*255);
        if (pack) {
            float dep; memcpy(&dep, g_src[1] + (size_t)i*4, 4);
            float c = coc_of(dep, 5.0f, 2.0f, 50.0f);
            d[i*4+3] = (uint8_t)(fminf(c, 1.0f)*255);
        } else d[i*4+3] = 255;
        ck += (uint64_t)(r*13 + g*7 + b*3);
    }
    *ckp += ck;
}

static void dof_job(int lo, int hi, uint64_t *ckp) {
    uint64_t ck = 0;
    const uint8_t *s = g_src[1]; uint8_t *d = g_dst[1];
    for (int i = lo; i < hi; i++) {
        float dep; memcpy(&dep, s + (size_t)i*4, 4);
        float c = coc_of(dep, 5.0f, 2.0f, 50.0f);
        uint16_t h = f32_to_f16(c); memcpy(d+(size_t)i*2, &h, 2);  /* 原生 R16F */
        ck += (uint64_t)(c * 101);
    }
    *ckp += ck;
}

static void bloom_job(int lo, int hi, uint64_t *ckp) {
    uint64_t ck = 0;
    const uint8_t *s = g_src[2]; uint8_t *d = g_dst[2];
    for (int i = lo; i < hi; i++) {
        uint16_t hh = (uint16_t)(s[i*2] | (s[i*2+1]<<8));
        float v = f16_to_f32(hh) - 0.3f; if (v < 0) v = 0;
        uint16_t o[4] = { f32_to_f16(v), f32_to_f16(v), f32_to_f16(v), f32_to_f16(1.0f) };
        memcpy(d + (size_t)i*8, o, 8);   /* RGBA16F */
        ck += (uint64_t)(v * 17);
    }
    *ckp += ck;
}

/* P_MRT：三个目标统一 RGBA16F，同一块内顺序写三个缓冲 */
static void mrt_job(int lo, int hi, uint64_t *ckp) {
    uint64_t ck = 0;
    const uint8_t *sh = g_src[0], *sd = g_src[1], *sl = g_src[2];
    uint8_t *dc = g_dst[0], *dk = g_dst[1], *db = g_dst[2];
    for (int i = lo; i < hi; i++) {
        float r = f16_to_f32((uint16_t)(sh[i*8+0] | (sh[i*8+1]<<8)));
        float g = f16_to_f32((uint16_t)(sh[i*8+2] | (sh[i*8+3]<<8)));
        float b = f16_to_f32((uint16_t)(sh[i*8+4] | (sh[i*8+5]<<8)));
        r = aces(r); g = aces(g); b = aces(b);
        uint16_t oc[4] = { f32_to_f16(r), f32_to_f16(g), f32_to_f16(b), f32_to_f16(1.0f) };
        memcpy(dc + (size_t)i*8, oc, 8);

        float dep; memcpy(&dep, sd + (size_t)i*4, 4);
        float c = coc_of(dep, 5.0f, 2.0f, 50.0f);
        uint16_t ok[4] = { f32_to_f16(c), 0, 0, f32_to_f16(1.0f) };
        memcpy(dk + (size_t)i*8, ok, 8);

        uint16_t lh = (uint16_t)(sl[i*2] | (sl[i*2+1]<<8));
        float v = f16_to_f32(lh) - 0.3f; if (v < 0) v = 0;
        uint16_t ob[4] = { f32_to_f16(v), f32_to_f16(v), f32_to_f16(v), f32_to_f16(1.0f) };
        memcpy(db + (size_t)i*8, ob, 8);

        ck += (uint64_t)(r*13 + g*7 + b*3 + c*101 + v*17);
    }
    *ckp += ck;
}

static void worker_job(int tid, int lo, int hi) {
    uint64_t ck = 0;
    switch (cur_pass) {
        case P_TONEMAP_SEP: tonemap_job(lo, hi, &ck); break;
        case P_DOF_SEP:     dof_job(lo, hi, &ck); break;
        case P_BLOOM:       bloom_job(lo, hi, &ck); break;
        case P_PACK:        tonemap_job(lo, hi, &ck); break;
        case P_MRT:         mrt_job(lo, hi, &ck); break;
    }
    g_check[tid] += ck;
}

static void *worker_main(void *arg) {
    int tid = *(int*)arg;
    for (;;) {
        barrier_wait();              /* 等派发 */
        atomic_thread_fence(memory_order_acquire);
        if (shutdown_flag) break;
        int pass = cur_pass;
        int chunk = (g_hi - g_lo + NW - 1) / NW;
        int lo = tid*chunk, hi = lo + chunk;
        if (hi > g_hi) hi = g_hi;
        if (lo < hi) worker_job(tid, lo, hi);
        (void)pass;
        barrier_wait();              /* 完成回执 */
    }
    return NULL;
}

/* 一次通道派发：固定开销（屏障往返）+ 真实工作，全部落在计时区内 */
static void dispatch(int pass, const uint8_t *s0, const uint8_t *s1, const uint8_t *s2,
                     uint8_t *d0, uint8_t *d1, uint8_t *d2, int n) {
    cur_pass = pass;
    g_src[0]=s0; g_src[1]=s1; g_src[2]=s2;
    g_dst[0]=d0; g_dst[1]=d1; g_dst[2]=d2;
    g_lo=0; g_hi=n;
    /* 保证任务描述的普通写先于屏障发布，worker 的 acquire 读与它建立 happens-before */
    atomic_thread_fence(memory_order_release);
    barrier_wait();  /* 派发 */
    barrier_wait();  /* 等齐 */
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
}

static Frame* frame_alloc(int w, int h) {
    Frame *f = calloc(1, sizeof *f);
    f->w=w; f->h=h; f->n=w*h;
    f->hdr   = malloc((size_t)f->n*B_HDR);
    f->depth = malloc((size_t)f->n*B_DEPTH);
    f->lum   = malloc((size_t)f->n*B_LUM);
    /* 按最大可能尺寸分配，各模式只用对应格式的字节，产生真实访存流量 */
    f->o_color = malloc((size_t)f->n*B_HI);
    f->o_coc   = malloc((size_t)f->n*B_HI);
    f->o_bloom = malloc((size_t)f->n*B_HI);
    srand(42);
    for (int i=0;i<f->n;i++) {
        for (int k=0;k<4;k++){ uint16_t hh=f32_to_f16((float)(rand()%2000)/500.0f);
            f->hdr[i*8+k*2]=(uint8_t)hh; f->hdr[i*8+k*2+1]=(uint8_t)(hh>>8); }
        float dep = 0.5f + (float)(rand()%1000)/100.0f;
        memcpy(f->depth+(size_t)i*4, &dep, 4);
        uint16_t lh = f32_to_f16((float)(rand()%1000)/800.0f);
        f->lum[i*2]=(uint8_t)lh; f->lum[i*2+1]=(uint8_t)(lh>>8);
    }
    return f;
}

/* 所有指针始终非空：未用槽位指向 dummy，未激活通道不会访问它们 */
static uint8_t dummy[64];

static void run_frame(Frame *f, const char *mode) {
    int n=f->n;
    if (!strcmp(mode,"sep")) { /* 3 通道：原生小格式，各自只读自己的输入 */
        dispatch(P_TONEMAP_SEP, f->hdr,  dummy,  dummy, f->o_color, dummy,    dummy, n);
        dispatch(P_DOF_SEP,     dummy,  f->depth,dummy,dummy,     f->o_coc,  dummy, n);
        dispatch(P_BLOOM,       dummy,  dummy,  f->lum,dummy,     dummy,     f->o_bloom, n);
    } else if (!strcmp(mode,"mrt")) { /* 1 通道：全量输入，3 目标统一 RGBA16F */
        dispatch(P_MRT, f->hdr, f->depth, f->lum, f->o_color, f->o_coc, f->o_bloom, n);
    } else { /* split：2 张。pass1 color+CoC 打包 RGBA8，pass2 bloom RGBA16F */
        dispatch(P_PACK,  f->hdr, f->depth, dummy, f->o_color, dummy, dummy, n);
        dispatch(P_BLOOM, dummy,  dummy,  f->lum, dummy,      dummy, f->o_bloom, n);
    }
}

static void pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

int main(int argc, char **argv) {
    const char *sync_env = getenv("BENCH_SYNC");
    if (sync_env && !strcmp(sync_env, "spin")) g_sync = SYNC_SPIN;
    barrier_init();
    int tids[NW];
    for (int i=0;i<NW;i++){
        tids[i]=i;
        pthread_create(&th[i],NULL,worker_main,&tids[i]);
        if (g_sync == SYNC_SPIN) pin_cpu(i);   /* worker 钉核 0..14 */
    }
    if (g_sync == SYNC_SPIN) pin_cpu(NW);      /* main 钉核 15 */

    if (argc >= 2 && !strcmp(argv[1],"calibrate")) {
        int reps = argc > 2 ? atoi(argv[2]) : 20000;
        double t0 = now_s();
        for (int i=0;i<reps;i++){ g_lo=g_hi=0; barrier_wait(); barrier_wait(); }
        double dt = now_s()-t0;
        printf("sync=%s c_fix_us=%.4f reps=%d\n",
               g_sync == SYNC_SPIN ? "spin" : "pthread", dt/reps*1e6, reps);
        shutdown_flag=1; barrier_wait();
        for (int i=0;i<NW;i++) pthread_join(th[i],NULL);
        return 0;
    }

    if (argc < 5) {
        fprintf(stderr,"usage: %s <sep|mrt|split> W H frames   (env BENCH_SYNC=spin|pthread)\n", argv[0]);
        return 2;
    }
    const char *mode = argv[2];
    int w=atoi(argv[3]), h=atoi(argv[4]), frames=atoi(argv[5]);
    Frame *f = frame_alloc(w,h);

    run_frame(f, mode); /* warmup，含缺页 */
    memset(g_check,0,sizeof g_check);
    double t0 = now_s();
    for (int i=0;i<frames;i++) run_frame(f, mode);
    double dt = now_s()-t0;
    uint64_t ck=0; for (int i=0;i<NW;i++) ck+=g_check[i];

    int reads, writes, passes;
    if (!strcmp(mode,"sep"))       { reads=B_HDR+B_DEPTH+B_LUM; writes=B_COLOR+B_COC+B_BLOOM; passes=3; }
    else if (!strcmp(mode,"mrt")) { reads=B_HDR+B_DEPTH+B_LUM; writes=3*B_HI; passes=1; }
    else                           { reads=B_HDR+B_DEPTH+B_LUM; writes=B_COLOR+B_BLOOM; passes=2; }

    printf("sync=%s mode=%s res=%dx%d frames=%d passes=%d bytes_per_px=%d(read=%d+write=%d) "
           "per_frame_ms=%.4f total_ms=%.2f checksum=%llu\n",
           g_sync == SYNC_SPIN ? "spin" : "pthread",
           mode, w, h, frames, passes, reads+writes, reads, writes,
           dt/frames*1e3, dt*1e3, (unsigned long long)ck);

    shutdown_flag=1; barrier_wait();
    for (int i=0;i<NW;i++) pthread_join(th[i],NULL);
    free(f->hdr); free(f->depth); free(f->lum);
    free(f->o_color); free(f->o_coc); free(f->o_bloom); free(f);
    return 0;
}
