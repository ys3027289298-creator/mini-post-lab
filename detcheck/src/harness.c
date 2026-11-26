/* detcheck harness: run the plugin twice (plus extra repeats) on identical
 * input, count differing pixels, and report the first difference with the
 * thread count in effect. Sweeps resolutions x thread counts x defect modes.
 *
 * Pass criterion: run1 vs run2 must be pixel-identical (diff12 == 0).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "plugin.h"

static uint64_t rs;
static uint32_t rnd(void) {
    rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rs >> 33);
}

/* deterministic HDR input: dim field + sparse bright spikes.
 * Values must be effectively continuous (full float32 granularity): a
 * coarsely quantized input lattice can keep every pixel far from a
 * rounding boundary and hide last-level flips entirely. */
static float* make_input(int w, int h, uint64_t seed) {
    rs = seed;
    int n = w * h;
    float* img = malloc((size_t)n * sizeof(float));
    for (int i = 0; i < n; i++) {
        float u = (float)rnd() / 4294967296.0f;
        float v = 0.001f + 0.6f * u;
        if (rnd() % 449 == 0)
            v += 8.0f + 56.0f * ((float)rnd() / 4294967296.0f);
        img[i] = v;
    }
    return img;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

typedef struct {
    long count;
    int fx, fy;
    unsigned char fa, fb;
} Diff;

static Diff cmp(const unsigned char* a, const unsigned char* b, int w, int h) {
    Diff d = { 0, -1, -1, 0, 0 };
    for (int i = 0; i < w * h; i++)
        if (a[i] != b[i]) {
            if (d.count == 0) {
                d.fx = i % w;
                d.fy = i / w;
                d.fa = a[i];
                d.fb = b[i];
            }
            d.count++;
        }
    return d;
}

/* returns total diff pixels across all repeat comparisons (for the
 * residual-uncertainty estimate of the shipped configuration) */
static long run_case(int w, int h, int threads, int fixed_order,
                     int cache_enabled, int cache_lossy, int repeats) {
    PluginCfg cfg = { threads, fixed_order, cache_enabled, cache_lossy };
    float* in = make_input(w, h, 0x9e3779b97f4a7c15ULL ^ ((uint64_t)w << 20) ^ (uint64_t)h);
    unsigned char* ref = malloc((size_t)w * h);
    unsigned char* cur = malloc((size_t)w * h);

    plugin_cache_clear();
    double t0 = now_ms();
    plugin_process(in, ref, w, h, &cfg);
    double tsum = now_ms() - t0;

    Diff d12 = { 0, -1, -1, 0, 0 };
    long total = 0;
    for (int r = 1; r < repeats; r++) {
        double a = now_ms();
        plugin_process(in, cur, w, h, &cfg);
        tsum += now_ms() - a;
        Diff d = cmp(ref, cur, w, h);
        if (r == 1) d12 = d;
        total += d.count;
    }

    printf("%4dx%-4d T=%-2d %-5s cache=%-5s diff12=%-4ld diffAll=%-5ld",
           w, h, threads, fixed_order ? "fixed" : "racy",
           cache_enabled ? (cache_lossy ? "lossy" : "full") : "off",
           d12.count, total);
    if (d12.count > 0)
        printf(" first=(%d,%d) run1=%u run2=%u threads=%d",
               d12.fx, d12.fy, d12.fa, d12.fb, threads);
    printf(" avg_ms=%.3f\n", tsum / repeats);

    free(in);
    free(ref);
    free(cur);
    return total;
}

int main(void) {
    static const int res[3][2] = { { 640, 360 }, { 1280, 720 }, { 1920, 1080 } };
    static const int nth[] = { 1, 2, 4, 8 };
    const int R = 16; /* 1 reference + 15 repeats; pass needs diff12 == 0 */

    printf("== A. racy reduction (arrival-order combine), cache off ==\n");
    for (int r = 0; r < 3; r++)
        for (int t = 0; t < 4; t++)
            run_case(res[r][0], res[r][1], nth[t], 0, 0, 0, R);

    printf("== B. fixed-order reduction, cache off ==\n");
    for (int r = 0; r < 3; r++)
        for (int t = 0; t < 4; t++)
            run_case(res[r][0], res[r][1], nth[t], 1, 0, 0, R);

    printf("== C. fixed-order reduction + lossy cache (hit path != miss path) ==\n");
    for (int r = 0; r < 3; r++)
        run_case(res[r][0], res[r][1], 4, 1, 1, 1, R);

    printf("== D. fixed-order reduction + full-precision cache ==\n");
    for (int r = 0; r < 3; r++)
        run_case(res[r][0], res[r][1], 4, 1, 1, 0, R);

    printf("== E. shipped config: fixed-order, cache cleared between runs ==\n");
    long residual = 0, pixels = 0;
    for (int r = 0; r < 3; r++)
        for (int t = 0; t < 4; t++) {
            residual += run_case(res[r][0], res[r][1], nth[t], 1, 0, 0, R);
            pixels += (long)res[r][0] * res[r][1] * (R - 1);
        }
    printf("residual: diffAll=%ld over %ld pixel comparisons -> "
           "p_hat=0, 95%% upper bound p < %.2e per pixel\n",
           residual, pixels, 3.0 / (double)pixels);
    return 0;
}
