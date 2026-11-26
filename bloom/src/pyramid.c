
/* pyramid: multi-level downsample bloom.
 *
 * cost model (level k = 0..m-1, level 0 is full resolution):
 *   pixels N_k = N0 / 4^k                       (halve W and H -> /4)
 *   blur   C_k = N_k * w      taps, w = 2r+1    (separable: common factor 2 omitted)
 *   total  T(m) = N0*w * sum_{k=0}^{m-1} 4^-k
 *               = (4/3) N0 w (1 - 4^-m)
 *
 * this file: pyramid implementation, self-tests, analytic level table,
 * and a frame-budget benchmark over three halo-radius presets. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define REF_W 1920
#define REF_H 1080
#define MAX_M 8
#define FRAMES 30
#define BUDGET 0.25          /* post-processing must stay <= 25% of frame */
#define RANGE_EPS 0.01       /* halo edge: response below 1% of peak */
#define THRESHOLD 1.0f

typedef struct { int w, h; float *d; } Mip;
typedef struct { long long bright, blur, down, up; } Taps;

static float *xmalloc(size_t n) {
    float *p = malloc(n);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

static void bright_pass(const float *src, float *dst, int n, float thr) {
    for (int i = 0; i < n; i++) {
        float v = src[i] - thr;
        dst[i] = v > 0.0f ? v : 0.0f;
    }
}

/* 2x2 box downsample; odd dimensions reuse the last row/column */
static void downsample(const float *s, int sw, int sh, float *d, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int y0 = 2 * y, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int x = 0; x < dw; x++) {
            int x0 = 2 * x, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            d[y * dw + x] = (s[y0 * sw + x0] + s[y0 * sw + x1] +
                             s[y1 * sw + x0] + s[y1 * sw + x1]) * 0.25f;
        }
    }
}

/* separable box blur, radius r, clamped edges; s and d may not alias */
static void box_blur(const float *s, float *d, float *tmp, int w, int h, int r) {
    int kw = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sum = 0.0f;
            for (int k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= w) xx = w - 1;
                sum += s[y * w + xx];
            }
            tmp[y * w + x] = sum / (float)kw;
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sum = 0.0f;
            for (int k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= h) yy = h - 1;
                sum += tmp[yy * w + x];
            }
            d[y * w + x] = sum / (float)kw;
        }
    }
}

/* bilinear upsample, accumulated additively into dst (same size as parent) */
static void upsample_add(const float *s, int sw, int sh, float *d, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = (int)floorf(fy);
        float wy = fy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) { y0 = 0; wy = 0.0f; }
        if (y1 >= sh) y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = (int)floorf(fx);
            float wx = fx - (float)x0;
            int x1 = x0 + 1;
            if (x0 < 0) { x0 = 0; wx = 0.0f; }
            if (x1 >= sw) x1 = sw - 1;
            float v = s[y0 * sw + x0] * (1.0f - wx) * (1.0f - wy) +
                      s[y0 * sw + x1] * wx * (1.0f - wy) +
                      s[y1 * sw + x0] * (1.0f - wx) * wy +
                      s[y1 * sw + x1] * wx * wy;
            d[y * dw + x] += v;
        }
    }
}

/* build the downsample chain: bright pass at level 0, then for each coarser
 * level a 2x2 downsample and a separable box blur. blur_level0 selects the
 * legacy path (full-res blur, the current implementation) vs the optimized
 * path (level 0 keeps raw bright-pass texels). */
static void build_chain(const float *input, int m, int r, int blur_level0,
                        Mip *mips, float *blur_tmp, Taps *tc) {
    int n0 = mips[0].w * mips[0].h;
    int kw = 2 * r + 1;
    bright_pass(input, mips[0].d, n0, THRESHOLD);
    if (tc) tc->bright += (long long)n0;
    if (blur_level0) {
        box_blur(mips[0].d, mips[0].d, blur_tmp, mips[0].w, mips[0].h, r);
        if (tc) tc->blur += (long long)n0 * 2 * kw;
    }
    for (int k = 1; k < m; k++) {
        downsample(mips[k - 1].d, mips[k - 1].w, mips[k - 1].h,
                   mips[k].d, mips[k].w, mips[k].h);
        if (tc) tc->down += (long long)mips[k].w * mips[k].h * 4;
        box_blur(mips[k].d, mips[k].d, blur_tmp, mips[k].w, mips[k].h, r);
        if (tc) tc->blur += (long long)mips[k].w * mips[k].h * 2 * kw;
    }
}

/* upsample one source level back to full resolution through a ping-pong
 * pair of full-res buffers; returns the buffer holding the result. */
static float *upsample_level(const Mip *mips, int from,
                             float *up_a, float *up_b, Taps *tc) {
    const float *src = mips[from].d;
    int sw = mips[from].w, sh = mips[from].h;
    float *cur = up_a, *nxt = up_b;
    for (int j = from; j >= 1; j--) {
        int pw = mips[j - 1].w, ph = mips[j - 1].h;
        memset(nxt, 0, (size_t)pw * ph * sizeof(float));
        upsample_add(src, sw, sh, nxt, pw, ph);
        if (tc) tc->up += (long long)pw * ph * 4;
        src = nxt; sw = pw; sh = ph;
        float *t = cur; cur = nxt; nxt = t;
    }
    return (float *)src;
}

/* one frame: build chain, then a single additive upsample chain. bilinear
 * upsampling is linear, so up(L1 + up(L2)) == up(L1) + up^2(L2): the cheap
 * chain produces the exact same sum as per-level independent upsampling.
 * dividing by m gives equal level weights summing to 1, preserving the energy
 * of a flat bright field. */
static void run_pyramid(const float *input, int m, int r, int blur_level0,
                        Mip *mips, float *blur_tmp,
                        float *out, Taps *tc) {
    int n0 = mips[0].w * mips[0].h;
    build_chain(input, m, r, blur_level0, mips, blur_tmp, tc);

    /* additive chain: coarsest level bubbles up into each parent in place */
    for (int k = m - 1; k >= 1; k--) {
        upsample_add(mips[k].d, mips[k].w, mips[k].h,
                     mips[k - 1].d, mips[k - 1].w, mips[k - 1].h);
        if (tc) tc->up += (long long)mips[k - 1].w * mips[k - 1].h * 4;
    }
    float scale = 1.0f / (float)m;
    for (int i = 0; i < n0; i++) out[i] = scale * mips[0].d[i];
}

static void free_mips(Mip *mips, int m) {
    for (int k = 0; k < m; k++) free(mips[k].d);
}

static int alloc_mips(Mip *mips, int m, int w, int h) {
    for (int k = 0; k < m; k++) {
        mips[k].w = k == 0 ? w : (mips[k - 1].w + 1) / 2;
        mips[k].h = k == 0 ? h : (mips[k - 1].h + 1) / 2;
        if (mips[k].w < 4 || mips[k].h < 4) return -1;
        mips[k].d = xmalloc((size_t)mips[k].w * mips[k].h * sizeof(float));
    }
    return 0;
}

/* closed-form blur cost T(m) in N0*w units */
static double total_cost(int m) {
    return (4.0 / 3.0) * (1.0 - pow(4.0, -m));
}

/* last-level share of the m-level finite total: 3 / (4^m - 1) */
static double last_share(int m) {
    return 3.0 / (pow(4.0, m) - 1.0);
}

/* widest halo in full-resolution pixels: r * 2^(m-1) */
static double halo_range(int m, int r) {
    return (double)r * pow(2.0, m - 1);
}

/* measured halo footprint: upsample the coarsest mip of an impulse back to
 * full res on its own, then take the Chebyshev radius at 1% of that level's
 * own peak. measuring against the composite peak would let the undiluted
 * level-0 texel dominate and hide every coarse halo. */
static int measure_radius(int m, int r) {
    Mip mips[MAX_M];
    if (alloc_mips(mips, m, REF_W, REF_H) != 0) { free_mips(mips, m); return -1; }
    int n0 = REF_W * REF_H;
    /* blur_tmp has to hold a full frame because box_blur writes into it */
    float *in = xmalloc((size_t)n0 * sizeof(float));
    float *blur_tmp = xmalloc((size_t)n0 * sizeof(float));
    float *up_a = xmalloc((size_t)n0 * sizeof(float));
    float *up_b = xmalloc((size_t)n0 * sizeof(float));
    memset(in, 0, (size_t)n0 * sizeof(float));
    int cx = REF_W / 2, cy = REF_H / 2;
    in[cy * REF_W + cx] = 100.0f;            /* single bright texel */
    build_chain(in, m, r, 0, mips, blur_tmp, NULL);

    float *full = upsample_level(mips, m - 1, up_a, up_b, NULL);
    float peak = full[cy * REF_W + cx];
    float edge = RANGE_EPS * peak;
    int radius = 0;
    for (int y = 0; y < REF_H; y++)
        for (int x = 0; x < REF_W; x++)
            if (full[y * REF_W + x] >= edge) {
                int d = abs(x - cx) > abs(y - cy) ? abs(x - cx) : abs(y - cy);
                if (d > radius) radius = d;
            }
    free(in); free(up_a); free(up_b); free(blur_tmp);
    free_mips(mips, m);
    return radius;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef struct {
    const char *name;
    int m, r, target_px;
    int blur_level0;          /* 1 = legacy full-res blur, 0 = optimized */
} Preset;

static double bench(const Preset *p, long long *taps_out) {
    Mip mips[MAX_M];
    if (alloc_mips(mips, p->m, REF_W, REF_H) != 0) {
        free_mips(mips, p->m);
        return -1.0;
    }
    int n0 = REF_W * REF_H;
    /* level 0 itself is blurred on the legacy anchor path, so the scratch
     * buffer always has to hold a full frame */
    float *in = xmalloc((size_t)n0 * sizeof(float));
    float *out = xmalloc((size_t)n0 * sizeof(float));
    float *blur_tmp = xmalloc((size_t)n0 * sizeof(float));
    for (int y = 0; y < REF_H; y++)           /* deterministic scene: gradient */
        for (int x = 0; x < REF_W; x++)       /* plus three bright rectangles */
            in[y * REF_W + x] = 0.3f + 0.4f * (float)x / REF_W;
    for (int b = 0; b < 3; b++) {
        int x0 = 200 + b * 500, y0 = 300 + b * 120;
        for (int y = y0; y < y0 + 80; y++)
            for (int x = x0; x < x0 + 80; x++)
                in[y * REF_W + x] = 4.0f;
    }
    double samples[FRAMES];
    Taps total = {0, 0, 0, 0};
    for (int f = 0; f < FRAMES; f++) {
        Taps one = {0, 0, 0, 0};
        double t0 = now_sec();
        run_pyramid(in, p->m, p->r, p->blur_level0,
                    mips, blur_tmp, out, &one);
        samples[f] = now_sec() - t0;
        total.bright += one.bright; total.blur += one.blur;
        total.down += one.down; total.up += one.up;
    }
    qsort(samples, FRAMES, sizeof(double), cmp_double);
    *taps_out = (total.bright + total.blur + total.down + total.up) / FRAMES;
    free(in); free(out); free(blur_tmp);
    free_mips(mips, p->m);
    return samples[FRAMES / 2] * 1000.0;      /* median ms per frame */
}

static int failures = 0;
static void check(const char *name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    /* ---- self tests ---- */
    float a[4] = {1.0f, 3.0f, 5.0f, 7.0f};
    float b[1];
    downsample(a, 2, 2, b, 1, 1);
    check("down_average", fabsf(b[0] - 4.0f) < 1e-6f);

    Mip mips[MAX_M];
    check("mip_dims", alloc_mips(mips, 3, REF_W, REF_H) == 0 &&
                      mips[1].w == 960 && mips[1].h == 540 &&
                      mips[2].w == 480 && mips[2].h == 270);
    int n0 = REF_W * REF_H;
    float *in = xmalloc((size_t)n0 * sizeof(float));
    float *out = xmalloc((size_t)n0 * sizeof(float));
    float *blur_tmp = xmalloc((size_t)n0 * sizeof(float));
    for (int i = 0; i < n0; i++) in[i] = 2.0f;   /* flat field above threshold */
    Taps tc = {0, 0, 0, 0};
    run_pyramid(in, 3, 1, 0, mips, blur_tmp, out, &tc);
    /* bright pass is subtractive (2.0 - 1.0 = 1.0); equal weights sum to 1,
     * so the whole chain must hand that 1.0 back untouched on a flat field */
    check("flat_energy_kept", fabsf(out[n0 / 2] - 1.0f) < 1e-3f);
    check("taps_counted", tc.bright > 0 && tc.blur > 0 && tc.down > 0 && tc.up > 0);
    free(in); free(out); free(blur_tmp);
    free_mips(mips, 3);

    int r3 = measure_radius(3, 1), r4 = measure_radius(4, 1), r5 = measure_radius(5, 1);
    check("radius_monotone", r3 > 0 && r4 > r3 && r5 > r4);
    check("model_matches_formula",
          fabs(total_cost(3) - (1.0 + 0.25 + 0.0625)) < 1e-9);

    /* ---- analytic level table ---- */
    printf("\n m   T(m)/(N0*w)   %% of T_inf   last-level share   kernel/mip width\n");
    for (int m = 1; m <= 7; m++) {
        double frac = total_cost(m) / (4.0 / 3.0);
        double rel_kw = (3.0 * pow(2.0, m - 1)) / REF_W; /* r=1, w=3 */
        printf("%2d %12.5f %11.2f%% %16.2f%% %15.2f%%\n",
               m, total_cost(m), 100.0 * frac, 100.0 * last_share(m), 100.0 * rel_kw);
    }
    double k_bound = log(61.0) / log(4.0);
    printf("\n5%% rule:  3/(4^m-1) >= 0.05  ->  m <= log4(61) = %.3f\n", k_bound);
    printf("m=3 last share %.2f%% (on the knee), m=4 last share %.2f%% (rejected)\n",
           100.0 * last_share(3), 100.0 * last_share(4));

    /* ---- frame-budget benchmark ----
     * flame-graph anchor: current post (m=3, r=4, 9-tap, blur at every level)
     * is 40% of the frame, so non-post cost G = 1.5 * post_cur and the 25%
     * budget requires post <= G/3. budget is judged on the op-count (tap)
     * model, which is what a GPU actually scales with; host wall time is shown
     * for reference only since it is bandwidth-bound on a CPU. */
    Preset anchor = { "current_r4_m3", 3, 4, 0, 1 };
    Preset presets[3] = {
        { "small_r1_m3",  3, 1, 4,  0 },
        { "medium_r1_m4", 4, 1, 8,  0 },
        { "large_r1_m5",  5, 1, 16, 0 },
    };
    long long taps_anchor = 0;
    double ms_anchor = bench(&anchor, &taps_anchor);
    double g_taps = 1.5 * (double)taps_anchor;   /* non-post work, in tap units */

    printf("\n%-16s %2s %2s %13s %10s %8s %9s %7s %7s %6s\n",
           "config", "m", "r", "taps/frame", "ms/frame", "post%",
           "range_px", "target", "budget", "range");
    double anchor_share = (double)taps_anchor / (g_taps + (double)taps_anchor);
    printf("%-16s %2d %2d %13lld %10.3f %7.2f%% %9s %7s %7s %6s\n",
           anchor.name, anchor.m, anchor.r, taps_anchor, ms_anchor,
           100.0 * anchor_share, "-", "-",
           anchor_share <= BUDGET ? "PASS" : "FAIL", "-");
    int all_pass = 1;
    for (int i = 0; i < 3; i++) {
        long long taps = 0;
        double ms = bench(&presets[i], &taps);
        double share = (double)taps / (g_taps + (double)taps);
        int measured = measure_radius(presets[i].m, presets[i].r);
        double modeled = halo_range(presets[i].m, presets[i].r);
        int budget_ok = share <= BUDGET;
        int range_ok = measured >= presets[i].target_px &&
                       modeled >= presets[i].target_px;
        all_pass &= budget_ok && range_ok;
        printf("%-16s %2d %2d %13lld %10.3f %7.2f%% %9.0f %7d %7s %6s\n",
               presets[i].name, presets[i].m, presets[i].r, taps, ms,
               100.0 * share, modeled, presets[i].target_px,
               budget_ok ? "PASS" : "FAIL", range_ok ? "PASS" : "FAIL");
        printf("  host-time post share %.2f%% (reference only); "
               "coarsest-level measured footprint %d px (1%% of its own peak)\n",
               100.0 * ms / (1.5 * ms_anchor + ms), measured);
    }
    check("all_presets_pass", all_pass);

    printf("\nall_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
