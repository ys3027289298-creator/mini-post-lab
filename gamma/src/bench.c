/* gamma cost curve: what the accuracy fix costs per pixel.
 *
 * Deviation alone does not settle the compensation; the per-pixel price does.
 * Two axes matter and they interact:
 *   - the branch in exact sRGB is data dependent: free when a frame is tonally
 *     uniform (whole band on one side of the knee), costly when the knee is
 *     crossed unpredictably;
 *   - powf is a libm call, so the "cheap" pure power is not the floor.
 *
 * Reported ns/px is the minimum over repetitions (interference only adds time),
 * measured single-threaded on the host CPU with -O2.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define K_KNEE 0.0031308f
#define A_SRGB 1.055f
#define B_SRGB 0.055f
#define MID_GRAY 0.18

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

typedef float (*encode_fn)(float);

/* the compensation under test */
static float enc_pure22(float x) { return powf(x, 1.0f / 2.2f); }

/* exact sRGB, toe first so dark pixels skip the powf entirely */
static float enc_piecewise(float x) {
    if (x <= K_KNEE) return 12.92f * x;
    return A_SRGB * powf(x, 1.0f / 2.4f) - B_SRGB;
}

/* sRGB curve without its toe, clamped at black; fmaxf keeps the powf calls
 * overlappable (a ?: here serialises them) */
static float enc_offset(float x) {
    return fmaxf(A_SRGB * powf(x, 1.0f / 2.4f) - B_SRGB, 0.0f);
}

#define LUT_N 2048
static float g_lut[LUT_N + 1];
static void build_lut(void) {
    for (int i = 0; i <= LUT_N; i++) {
        double x = (double)i / LUT_N;
        g_lut[i] = (float)(x <= K_KNEE ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055);
    }
}
static float enc_lut(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return g_lut[(int)(x * (float)LUT_N + 0.5f)];
}

static double best_ns(encode_fn f, const float* x, int n) {
    double best = 1e30;
    for (int r = 0; r < 15; r++) {
        double t0 = now();
        float acc = 0.0f;
        for (int i = 0; i < n; i++) acc += f(x[i]);
        double dt = now() - t0;
        if (acc < -1e30f) puts(""); /* keep the loop observable */
        if (dt < best) best = dt;
    }
    return best / (double)n * 1e9;
}

static double srgb_ref(double x) {
    if (x <= 0.0) return 0.0;
    if (x <= 0.0031308) return 12.92 * x;
    return 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    const int n = 1 << 21;
    float* px = malloc(sizeof(float) * (size_t)n);
    if (!px) return 1;
    build_lut();

    /* warm up caches and the powf paths before the first timing */
    for (int i = 0; i < n; i++) px[i] = 0.05f + 0.9f * (float)i / (float)n;
    (void)best_ns(enc_pure22, px, n);
    (void)best_ns(enc_piecewise, px, n);
    (void)best_ns(enc_offset, px, n);
    (void)best_ns(enc_lut, px, n);

    /* three exposure tiers: +/-1 EV around mid-gray 0.18 */
    const char* tier_name[3] = { "night", "indoor", "daylight" };
    const int tier_ev[3] = { -6, -3, 0 };
    double dark_lsb[3], band_lsb[3];

    printf("tier,band_lo,band_hi,toe_frac,dark_max_lsb,band_max_lsb,"
           "ns_pure22,ns_piecewise,ns_offset,ns_lut\n");
    for (int t = 0; t < 3; t++) {
        double lo = MID_GRAY * pow(2.0, tier_ev[t]) / 2.0;
        double hi = MID_GRAY * pow(2.0, tier_ev[t]) * 2.0;
        double mid = MID_GRAY * pow(2.0, tier_ev[t]);

        double dmx = 0.0, bmx = 0.0;
        for (int i = 0; i <= 200000; i++) {
            double x = lo + (hi - lo) * (double)i / 200000;
            double d = fabs(pow(x, 1.0 / 2.2) - srgb_ref(x)) * 255.0;
            if (d > bmx) bmx = d;
            if (x <= mid && d > dmx) dmx = d;
        }
        dark_lsb[t] = dmx;
        band_lsb[t] = bmx;

        /* fill with a log-uniform band, which is how exposure bands sit */
        unsigned s = 42u;
        for (int i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            double r = (double)(s >> 8) / 16777216.0;
            px[i] = (float)(lo * pow(hi / lo, r));
        }
        double toe = 0.0;
        if (lo < K_KNEE) toe = (K_KNEE - lo) / (hi - lo);
        if (toe > 1.0) toe = 1.0;

        printf("%s,%.7g,%.7g,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
               tier_name[t], lo, hi, toe, dmx, bmx,
               best_ns(enc_pure22, px, n),
               best_ns(enc_piecewise, px, n),
               best_ns(enc_offset, px, n),
               best_ns(enc_lut, px, n));
    }

    /* worst case for the branch: the knee crossed at random within one frame */
    printf("\nshadow_frac,ns_pure22,ns_piecewise,ns_offset,ns_lut\n");
    for (int fi = 0; fi <= 10; fi++) {
        double frac = fi / 10.0;
        unsigned s = 123456789u;
        for (int i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            float r = (float)(s >> 8) / 16777216.0f;
            s = s * 1664525u + 1013904223u;
            float u = (float)(s >> 8) / 16777216.0f;
            px[i] = (u < (float)frac) ? r * K_KNEE * 0.99f
                                      : K_KNEE + (1.0f - K_KNEE) * r;
        }
        printf("%.1f,%.3f,%.3f,%.3f,%.3f\n", frac,
               best_ns(enc_pure22, px, n),
               best_ns(enc_piecewise, px, n),
               best_ns(enc_offset, px, n),
               best_ns(enc_lut, px, n));
    }

    /* accuracy verdicts are deterministic; timing is not, so it only gets printed.
     * night and indoor dark halves exceed one LSB; daylight's dark half passes but
     * its full band does not (mid-tone dip). */
    check("pure22_dark_fails_night", dark_lsb[0] > 1.0);
    check("pure22_dark_fails_indoor", dark_lsb[1] > 1.0);
    check("pure22_dark_passes_daylight", dark_lsb[2] <= 1.0);
    check("pure22_band_fails_daylight", band_lsb[2] > 1.0);

    printf("all_ok=%d\n", failures == 0);
    free(px);
    return failures == 0 ? 0 : 1;
}
