/*
 * bench: post-process pipeline timing + skip-difference benchmark.
 *
 * Reuses the exact math from the three module self-tests:
 *   tonemap.c : ACES curve with exposure
 *   bloom.c   : bright-pass + separable box blur (clamped edges)
 *   dof.c     : thin-lens CoC, clamped blur radius
 *
 * For every load tier it runs the full pipeline (reference frame) and
 * variants with individual stages skipped, then reports per-stage time
 * and display-space image difference vs the reference.
 *
 * Output: human-readable lines plus CSV blocks on stdout.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---------------- shared math (copied verbatim from the modules) ------- */

static float aces(float x, float exposure) {
    float v = x * exposure;
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (v * (a * v + b)) / (v * (c * v + d) + e);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

static int bright_pass(const float* src, float* dst, int n, float threshold) {
    if (n <= 0 || threshold < 0.0f) return -1;
    for (int i = 0; i < n; i++) {
        float v = src[i] - threshold;
        dst[i] = v > 0.0f ? v : 0.0f;
    }
    return 0;
}

static int box_blur(const float* src, float* dst, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0 || r * 2 + 1 > w || r * 2 + 1 > h) return -1;
    float* tmp = (float*)malloc((size_t)w * h * sizeof(float));
    if (!tmp) return -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float s = 0.0f;
            for (int k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= w) xx = w - 1;
                s += src[y * w + xx];
            }
            tmp[y * w + x] = s / (float)(2 * r + 1);
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float s = 0.0f;
            for (int k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= h) yy = h - 1;
                s += tmp[yy * w + x];
            }
            dst[y * w + x] = s / (float)(2 * r + 1);
        }
    }
    free(tmp);
    return 0;
}

typedef struct {
    float focus;
    float aperture;
    float focal;
    float near_limit;
} Lens;

static float coc(const Lens* l, float depth) {
    if (depth <= 0.0f) return -1.0f;
    float d = depth;
    float num = l->aperture * l->focal * (d - l->focus);
    float den = d * (l->focus - l->focal);
    if (fabsf(den) < 1e-9f) return -1.0f;
    return fabsf(num / den);
}

static float blur_radius(const Lens* l, float depth, float k, float max_r) {
    float c = coc(l, depth);
    if (c < 0.0f) return -1.0f;
    float r = c * k;
    if (r > max_r) r = max_r;
    return r;
}

/* ---------------- deterministic test scene ----------------------------- */

static unsigned int rng_state;
static float rngf(void) {                       /* xorshift32, [0,1) */
    unsigned int x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return (float)(x & 0xffffff) / (float)0x1000000;
}

static float luminance(const float* rgb) {
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

/* HDR RGB scene + linear depth. Same content at every tier, resampled. */
static void make_scene(int w, int h, float* rgb, float* depth) {
    rng_state = 0x12345678u;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            float u = (float)x / (float)w;
            float v = (float)y / (float)h;
            /* smooth base gradient, mid-tone HDR radiance ~0.2..0.8 */
            float base = 0.18f + 0.5f * u + 0.12f * (1.0f - v);
            float r = base * (0.95f + 0.1f * rngf());
            float g = base * (0.95f + 0.1f * rngf());
            float b = base * (0.95f + 0.1f * rngf());
            /* two compact highlight disks, radiance up to ~6 (bloom source) */
            float d1 = hypotf(u - 0.30f, v - 0.35f);
            float d2 = hypotf(u - 0.72f, v - 0.60f);
            if (d1 < 0.022f) { float k = 1.0f - d1 / 0.022f; r += 5.0f * k; g += 4.6f * k; b += 3.8f * k; }
            if (d2 < 0.018f) { float k = 1.0f - d2 / 0.018f; r += 4.2f * k; g += 4.4f * k; b += 5.2f * k; }
            rgb[3 * i + 0] = r;
            rgb[3 * i + 1] = g;
            rgb[3 * i + 2] = b;
            /* depth 1..12, focus plane at 5: foreground and background defocus */
            depth[i] = 1.0f + 11.0f * v + 0.6f * sinf(6.2832f * u);
            if (depth[i] < 0.5f) depth[i] = 0.5f;
        }
    }
}

/* ---------------- pipeline ---------------------------------------------- */

typedef struct {
    int w, h, n;
    int bloom_r, dof_r;
    float exposure, bloom_threshold, bloom_gain;
    Lens lens;
    float k, max_r;
} Tier;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

/*
 * mask bits: 1 = skip tonemap, 2 = skip bloom, 4 = skip dof.
 * Stage timings (ms) are written into t_tm/t_bl/t_dof when the stage runs.
 * Output is display-space RGB in [0,1].
 */
static void run_pipeline(const Tier* t, const float* scene_rgb, const float* depth,
                         int mask, float* out,
                         double* t_tm, double* t_bl, double* t_dof) {
    int n = t->n;
    float *work = (float*)malloc((size_t)3 * n * sizeof(float));
    float *lum = (float*)malloc((size_t)n * sizeof(float));
    float *bright = (float*)malloc((size_t)n * sizeof(float));
    float *blurred = (float*)malloc((size_t)n * sizeof(float));
    double a, b;

    memcpy(work, scene_rgb, (size_t)3 * n * sizeof(float));

    /* ---- tonemap: ACES per channel (tonemap.c) ---- */
    if (!(mask & 1)) {
        a = now_ms();
        for (int i = 0; i < n; i++) {
            work[3*i+0] = aces(work[3*i+0], t->exposure);
            work[3*i+1] = aces(work[3*i+1], t->exposure);
            work[3*i+2] = aces(work[3*i+2], t->exposure);
        }
        b = now_ms();
        *t_tm = b - a;
    } else {
        *t_tm = 0.0;
        for (int i = 0; i < 3 * n; i++) work[i] = clamp01(work[i]); /* display clamp */
    }

    /* ---- bloom: bright-pass on luminance, separable box blur, composite (bloom.c) ---- */
    if (!(mask & 2)) {
        a = now_ms();
        for (int i = 0; i < n; i++) lum[i] = luminance(&work[3 * i]);
        memset(bright, 0, (size_t)n * sizeof(float));
        bright_pass(lum, bright, n, t->bloom_threshold);
        box_blur(bright, blurred, t->w, t->h, t->bloom_r);
        for (int i = 0; i < n; i++) {
            float add = t->bloom_gain * blurred[i];
            work[3*i+0] = clamp01(work[3*i+0] + add);
            work[3*i+1] = clamp01(work[3*i+1] + add);
            work[3*i+2] = clamp01(work[3*i+2] + add);
        }
        b = now_ms();
        *t_bl = b - a;
    } else {
        *t_bl = 0.0;
    }

    /* ---- dof: CoC + clamped radius per pixel (dof.c), then separable box blur ---- */
    if (!(mask & 4)) {
        a = now_ms();
        float radius_sum = 0.0f;
        for (int i = 0; i < n; i++) {
            float r = blur_radius(&t->lens, depth[i], t->k, t->max_r);
            radius_sum += r;
        }
        int r = (int)(radius_sum / n + 0.5f);   /* mean applied radius, clamped like module */
        if (r < 1) r = 1;
        if (r > t->dof_r) r = t->dof_r;
        for (int ch = 0; ch < 3; ch++) {
            for (int i = 0; i < n; i++) lum[i] = work[3 * i + ch];
            box_blur(lum, blurred, t->w, t->h, r);
            for (int i = 0; i < n; i++) work[3 * i + ch] = blurred[i];
        }
        b = now_ms();
        *t_dof = b - a;
    } else {
        *t_dof = 0.0;
    }

    memcpy(out, work, (size_t)3 * n * sizeof(float));
    free(work); free(lum); free(bright); free(blurred);
}

/* ---------------- difference metrics ------------------------------------ */

typedef struct { double rmse, psnr, mad, maxd, affected; } Diff;

static Diff compare(const Tier* t, const float* ref, const float* img) {
    long long m = 3LL * t->n;
    double sse = 0.0, sad = 0.0, maxd = 0.0, aff = 0.0;
    for (long long i = 0; i < m; i++) {
        double d = fabs((double)img[i] - (double)ref[i]);
        sse += d * d;
        sad += d;
        if (d > maxd) maxd = d;
        if (d > 0.01) aff += 1.0;
    }
    Diff q;
    q.rmse = sqrt(sse / (double)m);
    q.psnr = q.rmse > 1e-12 ? 20.0 * log10(1.0 / q.rmse) : 99.0;
    q.mad = sad / (double)m;
    q.maxd = maxd;
    q.affected = aff / (double)m;
    return q;
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double median(double* v, int k) {
    qsort(v, k, sizeof(double), cmp_double);
    return v[k / 2];
}

int main(void) {
    /* Three load tiers; L2 resolution calibrated so full pipeline ~= 5.2 ms. */
    Tier tiers[3] = {
        { 384, 216, 384*216, 2, 2, 1.0f, 0.7f, 0.25f, {5.0f, 2.0f, 50.0f, 0.1f}, 8.0f, 3.0f },
        { 552, 311, 552*311, 2, 2, 1.0f, 0.7f, 0.25f, {5.0f, 2.0f, 50.0f, 0.1f}, 8.0f, 3.0f },
        { 640, 360, 640*360, 2, 2, 1.0f, 0.7f, 0.25f, {5.0f, 2.0f, 50.0f, 0.1f}, 8.0f, 3.0f },
    };
    const char* tier_name[3] = { "L1_384x216", "L2_552x311", "L3_640x360" };
    /* masks: reference, skip tonemap, skip bloom, skip dof, skip bloom+dof */
    int masks[5] = { 0, 1, 2, 4, 6 };
    const char* mask_name[5] = { "full", "skip_tonemap", "skip_bloom", "skip_dof", "skip_bloom_dof" };

    const int FRAMES = 31, WARMUP = 5;
    const double THRESHOLD = 0.02;   /* perceptual threshold: display-space RMSE */

    printf("threshold_rmse=%.4f (PSNR %.1f dB), frames=%d warmup=%d, median reported\n",
           THRESHOLD, 20.0 * log10(1.0 / THRESHOLD), FRAMES, WARMUP);
    printf("CSV_TIME,tier,stage,ms\n");
    printf("CSV_DIFF,tier,variant,rmse,psnr,mad,maxd,affected_frac,skippable\n");

    for (int ti = 0; ti < 3; ti++) {
        Tier* t = &tiers[ti];
        int n = t->n;
        float *scene = (float*)malloc((size_t)3 * n * sizeof(float));
        float *depth = (float*)malloc((size_t)n * sizeof(float));
        float *ref = (float*)malloc((size_t)3 * n * sizeof(float));
        float *out = (float*)malloc((size_t)3 * n * sizeof(float));
        make_scene(t->w, t->h, scene, depth);

        double tm[FRAMES], bl[FRAMES], df[FRAMES];
        for (int f = 0; f < WARMUP + FRAMES; f++) {
            double a, b, c;
            run_pipeline(t, scene, depth, 0, ref, &a, &b, &c);
            if (f >= WARMUP) { tm[f-WARMUP] = a; bl[f-WARMUP] = b; df[f-WARMUP] = c; }
        }
        double m_tm = median(tm, FRAMES), m_bl = median(bl, FRAMES), m_df = median(df, FRAMES);
        printf("CSV_TIME,%s,tonemap,%.4f\n", tier_name[ti], m_tm);
        printf("CSV_TIME,%s,bloom,%.4f\n", tier_name[ti], m_bl);
        printf("CSV_TIME,%s,dof,%.4f\n", tier_name[ti], m_df);
        printf("CSV_TIME,%s,total,%.4f\n", tier_name[ti], m_tm + m_bl + m_df);

        for (int mi = 0; mi < 5; mi++) {
            Diff d = {0};
            for (int f = 0; f < WARMUP + FRAMES; f++) {
                double a, b, c;
                run_pipeline(t, scene, depth, masks[mi], out, &a, &b, &c);
                if (f == WARMUP) d = compare(t, ref, out);   /* deterministic: one diff is enough */
            }
            printf("CSV_DIFF,%s,%s,%.5f,%.2f,%.5f,%.5f,%.5f,%d\n",
                   tier_name[ti], mask_name[mi], d.rmse, d.psnr, d.mad, d.maxd,
                   d.affected, masks[mi] != 0 && d.rmse <= THRESHOLD);
        }

        free(scene); free(depth); free(ref); free(out);
    }
    return 0;
}
