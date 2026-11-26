
/* tonemap: Reinhard and ACES curve with exposure, plus histogram stats. */
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct { int n; float lo, hi; } Hist;

static float reinhard(float x, float exposure) {
    float v = x * exposure;
    return v / (1.0f + v);
}

static float aces(float x, float exposure) {
    float v = x * exposure;
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (v * (a * v + b)) / (v * (c * v + d) + e);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

/* histogram of luminance over [lo,hi], n bins */
static int histogram(const float* lum, int count, Hist* h, int* bins) {
    if (count <= 0 || h->n <= 0 || h->hi <= h->lo) return -1;
    for (int i = 0; i < h->n; i++) bins[i] = 0;
    for (int i = 0; i < count; i++) {
        float t = (lum[i] - h->lo) / (h->hi - h->lo);
        if (t < 0.0f) t = 0.0f;
        if (t > 0.9999f) t = 0.9999f;
        bins[(int)(t * h->n)]++;
    }
    return 0;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    check("reinhard_zero", fabsf(reinhard(0.0f, 1.0f)) < 1e-6f);
    check("reinhard_monotone", reinhard(1.0f, 1.0f) < reinhard(2.0f, 1.0f));
    check("reinhard_bounded", reinhard(1000.0f, 1.0f) < 1.0f);
    check("reinhard_half", fabsf(reinhard(1.0f, 1.0f) - 0.5f) < 1e-6f);

    check("aces_zero", fabsf(aces(0.0f, 1.0f)) < 1e-6f);
    check("aces_bounded", aces(1000.0f, 1.0f) <= 1.0f && aces(1000.0f, 1.0f) > 0.9f);
    check("aces_monotone", aces(1.0f, 1.0f) < aces(2.0f, 1.0f));

    /* exposure scales input: doubling exposure is same as doubling radiance */
    check("exposure_equiv", fabsf(reinhard(2.0f, 1.0f) - reinhard(1.0f, 2.0f)) < 1e-6f);

    float lum[4] = { 0.0f, 0.25f, 0.5f, 1.0f };
    Hist h = { 4, 0.0f, 1.0f };
    int bins[4];
    check("hist_ok", histogram(lum, 4, &h, bins) == 0);
    int total = 0;
    for (int i = 0; i < 4; i++) total += bins[i];
    check("hist_total", total == 4);
    check("hist_spread", bins[0] >= 1 && bins[3] >= 1);

    Hist bad = { 4, 1.0f, 0.0f };
    check("hist_bad_range", histogram(lum, 4, &bad, bins) == -1);
    check("hist_empty", histogram(lum, 0, &h, bins) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
