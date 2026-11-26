/*
 * neg_bias: measure negative-value production of the three pre-tonemap
 * subtraction stages and back out the minimum bias that keeps the negative
 * pixel ratio at or below 1/10000, then check that bias against the
 * "mean lift <= half an 8-bit quantization step (1/512)" budget.
 *
 * Stages:
 *   1. bloom bright-pass : d = L - threshold            (mirrors bloom.c)
 *   2. unsharp mask      : d = center - box_blur(center)
 *   3. chrom-aberration  : d = R - B  (fringe / channel difference)
 *
 * No real pixel dump ships with the repo, so three deterministic 1080p HDR
 * scenes (dark / mid / bright) are synthesized as stand-ins. All assumptions
 * and constants are printed by --params and documented in report.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define W 1920
#define H 1080
#define N (W * H)
#define TARGET_RATIO 1e-4
#define TARGET_CNT 207                 /* floor(N * 1e-4) */
#define HALF_QSTEP (1.0 / 512.0)      /* half of 1/255 in LDR space */
#define THRESHOLD 1.0f                /* bloom bright-pass threshold (HDR) */
#define BLUR_R 1                      /* unsharp box blur radius */
#define US_AMOUNT 0.5f                /* unsharp composite weight */
#define BLOOM_GAIN 0.3f               /* bloom composite weight */
#define CA_GAIN 0.5f                  /* CA correction composite weight */
#define SWEEP 25

/* ---------- deterministic RNG (xorshift32 + Box-Muller) ---------- */
static unsigned int rng_state;
static float uni(void) {
    unsigned int x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return (x >> 8) * (1.0f / 16777216.0f);   /* 24-bit uniform in [0,1) */
}
static float gauss(void) {
    float u1 = uni(), u2 = uni();
    if (u1 < 1e-9f) u1 = 1e-9f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

/* ---------- reference tonemap (same curve family as tonemap.c) ---------- */
/* reference tonemap (same curve family as tonemap.c); the display-stage
 * clamp happens on the radiance input, not on the curve output, because
 * Reinhard is not monotone across zero (x/(1+x) flips sign for x < -1) */
static float reinhard(float x) {
    if (x < 0.0f) x = 0.0f;
    return x / (1.0f + x);
}
static float clamp0(float x) { return x > 0.0f ? x : 0.0f; }

/* ---------- separable box blur, clamped edges (same logic as bloom.c) --- */
static void box_blur(const float *src, float *dst) {
    float *tmp = malloc(sizeof(float) * N);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float s = 0.0f;
            for (int k = -BLUR_R; k <= BLUR_R; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0; else if (xx >= W) xx = W - 1;
                s += src[y * W + xx];
            }
            tmp[y * W + x] = s / (float)(2 * BLUR_R + 1);
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float s = 0.0f;
            for (int k = -BLUR_R; k <= BLUR_R; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0; else if (yy >= H) yy = H - 1;
                s += tmp[yy * W + x];
            }
            dst[y * W + x] = s / (float)(2 * BLUR_R + 1);
        }
    }
    free(tmp);
}

/* ---------- scene synthesis ---------- */
typedef struct { const char *name; float base; unsigned int seed; } Scene;

static void build_scene(const Scene *sc, float *L, float *R, float *Bch) {
    rng_state = sc->seed;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            /* low-frequency pedestal: 0.75..1.0 of scene base */
            float ped = 0.75f + 0.25f * sinf(x * 0.011f) * cosf(y * 0.007f);
            float v = sc->base * ped;
            /* lognormal sensor noise, mean preserving (sigma=0.28) */
            v *= expf(0.28f * gauss() - 0.5f * 0.28f * 0.28f);
            float u = uni();
            if (u < 0.008f) v *= 0.05f;                 /* dark spot / dust */
            else if (u < 0.020f) v *= 4.0f + 36.0f * uni(); /* highlight */
            L[i] = v;
        }
    }
    /* channels: per-channel gain noise + 1px lateral split on strong
     * horizontal gradients, modelling sensor crosstalk and CA fringe */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            int xm = x > 0 ? x - 1 : 0;
            int xp = x < W - 1 ? x + 1 : W - 1;
            float gx = L[y * W + xp] - L[y * W + xm];
            float fringe = fabsf(gx) > 0.35f * sc->base ? 1.0f : 0.0f;
            float r = L[y * W + (fringe ? xm : x)] * (1.0f + 0.04f * gauss());
            float b = L[y * W + (fringe ? xp : x)] * (1.0f + 0.04f * gauss());
            R[i] = r;
            Bch[i] = b;
        }
    }
}

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/*
 * End-to-end comparison on the composite frame
 *   out = reinhard(L + BLOOM_GAIN*bright + US_AMOUNT*unsharp + CA_GAIN*ca)
 * signed: every subtraction keeps its sign, only the final display value
 *         is clamped into [0,1] (tonemap curves already bound the top end);
 * clamped: each subtraction is clamped to >= 0 (the buggy behavior that
 *          renders the negative pixels pure black).
 */
static void pipeline_compare(const Scene *sc, const float *L, const float *R,
                             const float *Bc, const float *blur) {
    double se = 0.0;
    long black_clamped = 0, black_signed = 0, clipped_signed = 0;
    double clipped_energy = 0.0;
    for (int i = 0; i < N; i++) {
        float bright = clamp0(L[i] - THRESHOLD);
        float us_s = L[i] - blur[i];
        float ca_s = R[i] - Bc[i];
        float comp_s = L[i] + BLOOM_GAIN * bright + US_AMOUNT * us_s
                       + CA_GAIN * ca_s;
        float comp_c = L[i] + BLOOM_GAIN * bright
                       + US_AMOUNT * clamp0(us_s) + CA_GAIN * clamp0(ca_s);
        long neg_before_tm = comp_s < 0.0f ? 1 : 0;
        float out_s = reinhard(comp_s);   /* display clamp inside */
        float out_c = reinhard(comp_c);
        if (neg_before_tm) { clipped_signed++; clipped_energy += -comp_s; }
        if (out_s == 0.0f) black_signed++;
        if (out_c == 0.0f) black_clamped++;
        float e = out_s - out_c;
        se += (double)e * e;
    }
    printf("%s,pipeline,rmse_tonemapped=%.6f,black_clamped=%ld,black_signed=%ld,"
           "neg_before_tonemap=%ld,clipped_hdr_energy=%.6f\n",
           sc->name, sqrt(se / N), black_clamped, black_signed,
           clipped_signed, clipped_energy / N);
}

typedef struct {
    const char *stage;
    long neg;
    float min_neg;       /* most negative value */
    float mean_neg;      /* mean over negative pixels */
    float loss_per_px;   /* mean |negative| over ALL pixels (clipped energy) */
    float bstar;         /* minimum bias meeting TARGET_RATIO */
    float lift_at_bstar; /* realized tonemapped mean lift at bstar */
    float b_budget;      /* bias at which lift exhausts HALF_QSTEP */
    long neg_at_budget;  /* negative pixels still remaining at b_budget */
    int pass;
} Stats;

/* mean tonemapped frame value for composite base + gain * clamp(d + bias) */
static float tonemapped_mean(const float *base, const float *d, float gain,
                             float bias) {
    double s = 0.0;
    for (int i = 0; i < N; i++)
        s += reinhard(base[i] + gain * clamp0(d[i] + bias));
    return (float)(s / N);
}

static void analyze(const char *name, const float *base, const float *d,
                    float gain, const char *csv_path, Stats *st) {
    st->stage = name;
    double sum_abs_neg = 0.0, sum_neg = 0.0;
    long neg = 0;
    float minv = 0.0f;
    float *negvals = malloc(sizeof(float) * N);
    for (int i = 0; i < N; i++) {
        if (d[i] < 0.0f) {
            negvals[neg++] = d[i];
            sum_abs_neg += -d[i];
            sum_neg += d[i];
            if (d[i] < minv) minv = d[i];
        }
    }
    st->neg = neg;
    st->min_neg = neg ? minv : 0.0f;
    st->mean_neg = neg ? (float)(sum_neg / neg) : 0.0f;
    st->loss_per_px = (float)(sum_abs_neg / N);

    qsort(negvals, neg, sizeof(float), cmp_float);
    if (neg == 0) {
        st->bstar = 0.0f;
    } else {
        long k = neg - 1 < TARGET_CNT ? neg - 1 : TARGET_CNT;
        st->bstar = -negvals[k];   /* equality lands on 0, not negative */
    }

    float m0 = tonemapped_mean(base, d, gain, 0.0f);
    st->lift_at_bstar = tonemapped_mean(base, d, gain, st->bstar) - m0;
    st->pass = st->lift_at_bstar <= HALF_QSTEP;

    /* bias at which the lift budget is fully spent: binary search,
     * lift is monotone non-decreasing in bias */
    float lo = 0.0f, hi = neg ? -minv * 1.05f : 0.0f;
    for (int it = 0; it < 40; it++) {
        float mid = 0.5f * (lo + hi);
        float lift = tonemapped_mean(base, d, gain, mid) - m0;
        if (lift <= HALF_QSTEP) lo = mid; else hi = mid;
    }
    st->b_budget = lo;
    st->neg_at_budget = 0;
    for (int i = 0; i < N; i++) if (d[i] + st->b_budget < 0.0f) st->neg_at_budget++;

    /* sweep curves: negative ratio and realized lift vs bias.
     * geometric spacing keeps resolution near b=0 where curves are steep */
    FILE *f = fopen(csv_path, "w");
    fprintf(f, "bias,neg_ratio,tonemapped_lift\n");
    float bmax = neg ? -minv * 1.05f : 0.0f;
    for (int s = 0; s <= SWEEP; s++) {
        float b;
        if (s == 0) b = 0.0f;
        else {
            float t = (float)(s - 1) / (SWEEP - 1);
            b = bmax * powf(10.0f, -4.0f * (1.0f - t));
        }
        long c = 0;
        for (int i = 0; i < N; i++) if (d[i] + b < 0.0f) c++;
        float lift = tonemapped_mean(base, d, gain, b) - m0;
        fprintf(f, "%.6f,%.8f,%.8f\n", b, (double)c / N, lift);
    }
    fclose(f);
    free(negvals);
}

int main(void) {
    Scene scenes[3] = {
        {"dark",   0.05f, 101u},
        {"mid",    0.35f, 202u},
        {"bright", 1.50f, 303u},
    };

    float *L = malloc(sizeof(float) * N);
    float *R = malloc(sizeof(float) * N);
    float *Bc = malloc(sizeof(float) * N);
    float *blur = malloc(sizeof(float) * N);
    float *d = malloc(sizeof(float) * N);

    printf("params: W=%d H=%d N=%d target_ratio=1e-4 target_count=%d "
           "half_qstep=%.6f threshold=%.2f blur_r=%d\n",
           W, H, N, TARGET_CNT, HALF_QSTEP, THRESHOLD, BLUR_R);

    for (int s = 0; s < 3; s++) {
        build_scene(&scenes[s], L, R, Bc);
        float lmin = L[0], lmax = L[0], cmax = 0.0f;
        for (int i = 0; i < N; i++) {
            if (L[i] < lmin) lmin = L[i];
            if (L[i] > lmax) lmax = L[i];
            float mc = R[i] > Bc[i] ? R[i] : Bc[i];
            if (mc > cmax) cmax = mc;
        }
        printf("%s,range,Lmin=%.6f,Lmax=%.6f,channel_max=%.6f,"
               "bound_bright=%.6f,bound_ca=%.6f\n",
               scenes[s].name, lmin, lmax, cmax, -THRESHOLD, -cmax);
        char path[256];

        /* stage 1: bright pass L - T */
        for (int i = 0; i < N; i++) d[i] = L[i] - THRESHOLD;
        snprintf(path, sizeof(path), "out/%s_bloom.csv", scenes[s].name);
        Stats st_bloom;
        analyze("bright_pass", L, d, BLOOM_GAIN, path, &st_bloom);

        /* stage 2: unsharp center - blur */
        box_blur(L, blur);
        for (int i = 0; i < N; i++) d[i] = L[i] - blur[i];
        snprintf(path, sizeof(path), "out/%s_sharpen.csv", scenes[s].name);
        Stats st_sharp;
        analyze("unsharp", L, d, US_AMOUNT, path, &st_sharp);

        /* stage 3: chromatic aberration R - B */
        for (int i = 0; i < N; i++) d[i] = R[i] - Bc[i];
        snprintf(path, sizeof(path), "out/%s_ca.csv", scenes[s].name);
        Stats st_ca;
        analyze("ca", L, d, CA_GAIN, path, &st_ca);

        Stats all[3] = {st_bloom, st_sharp, st_ca};
        for (int k = 0; k < 3; k++) {
            printf("%s,%s,neg=%ld,ratio=%.6f,min=%.6f,mean_neg=%.6f,"
                   "loss_per_px=%.6f,bstar=%.6f,lift=%.6f,pass=%d,"
                   "b_budget=%.6f,neg_at_budget=%ld\n",
                   scenes[s].name, all[k].stage, all[k].neg,
                   (double)all[k].neg / N, all[k].min_neg, all[k].mean_neg,
                   all[k].loss_per_px, all[k].bstar,
                   all[k].lift_at_bstar, all[k].pass,
                   all[k].b_budget, all[k].neg_at_budget);
        }
        pipeline_compare(&scenes[s], L, R, Bc, blur);
    }
    free(L); free(R); free(Bc); free(blur); free(d);
    return 0;
}
