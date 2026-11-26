/*
 * clip_tail: separate the two readback loss terms.
 *   interior term : |T(q_b(x)) - T(x)| for x < xmax  (quantization grid)
 *   clipping term : |T(x) - T(xmax)|   for x >= xmax (HDR tail collapsed)
 * In the current scene xmax is the frame max so clipping only touches the
 * head pixel itself; model a realistic fixed-headroom readback (xmax fixed)
 * to expose the clipping term at the bright tier.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#define W 256
#define H 256
#define NPIX (W * H)
#define TOL 0.00392156862745

static float reinhard(float v) { return v / (1.0f + v); }
static float aces(float v) {
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (v * (a * v + b)) / (v * (c * v + d) + e);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}
static float (*T[2])(float) = {reinhard, aces};
static const char* curve_name[2] = {"reinhard", "aces"};

static void make_scene(int tier, float* img) {
    const float base[3] = {0.02f, 0.25f, 1.20f};
    const float span[3] = {0.30f, 2.50f, 8.00f};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = (float)x / (W - 1), v = (float)y / (H - 1);
            float r = base[tier] + span[tier] * u;
            if ((x * 7 + y * 13) % 64 == 0) r *= 3.0f;
            r *= 0.95f + 0.10f * v;
            img[y * W + x] = r;
        }
}

static float quantize_b(float x, int b, float xmax) {
    if (x <= 0.0f) return 0.0f;
    if (x >= xmax) return xmax;
    double levels = (double)((1ULL << b) - 1);
    long q = lround((double)x / (double)xmax * levels);
    return (float)((double)q / levels * (double)xmax);
}

int main(void) {
    FILE* f = fopen("analysis/out/clip_tail.csv", "w");
    fprintf(f, "tier,curve,headroom,interior_max,clip_max,clip_pixel_fraction\n");

    /* fixed headroom values: a pipeline that always reads back into [0,8] */
    const float headroom[3] = {1.0f, 8.0f, 8.0f};

    for (int tier = 0; tier < 3; tier++) {
        float* hdr = malloc(sizeof(float) * NPIX);
        make_scene(tier, hdr);
        float xmax = headroom[tier];
        for (int ci = 0; ci < 2; ci++) {
            float interior_max = 0.0f, clip_max = 0.0f;
            int clip_n = 0;
            for (int i = 0; i < NPIX; i++) {
                if (hdr[i] >= xmax) {
                    float d = fabsf(T[ci](hdr[i]) - T[ci](xmax));
                    if (d > clip_max) clip_max = d;
                    clip_n++;
                } else {
                    float q = quantize_b(hdr[i], 8, xmax);
                    float d = fabsf(T[ci](q) - T[ci](hdr[i]));
                    if (d > interior_max) interior_max = d;
                }
            }
            printf("%-7s %-9s headroom=%4.1f interior_max=%.6f clip_max=%.6f "
                   "clip_frac=%.4f clip_over_tol=%d\n",
                   tier == 0 ? "dark" : tier == 1 ? "mid" : "bright",
                   curve_name[ci], xmax, interior_max, clip_max,
                   (double)clip_n / NPIX, clip_max > TOL);
            fprintf(f, "%d,%s,%.2f,%.8f,%.8f,%.6f\n", tier, curve_name[ci],
                    xmax, interior_max, clip_max, (double)clip_n / NPIX);
        }
        free(hdr);
    }
    fclose(f);
    return 0;
}
