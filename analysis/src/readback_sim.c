/*
 * readback_sim: preview (readback, quantized) vs export (offline, full precision)
 *
 * Two paths over the same HDR radiance frame:
 *   export  : L_tonemap = T(x * exposure)                       (float32)
 *   preview : q_b(x) = clamp(round(x * (2^b-1)),0,2^b-1)/(2^b-1)   (GPU readback)
 *             P_tonemap = T(q_b(x) * exposure)
 *
 * The readback quantizes the *pre-tonemap* buffer, so the two paths feed the
 * tone mapper different input ranges: q_b collapses the top tail (everything
 * above xmax maps to xmax) and snaps the interior onto a uniform grid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define W 256
#define H 256
#define NPIX (W * H)
#define NLEVELS 3
#define NDEPTHS 6
#define TOL 0.00392156862745f   /* 1/255: one final-image quantization level */

static const int depths[NDEPTHS] = {8, 10, 12, 14, 16, 32};

/* ---- tone curves (mirror tonemap/src/tonemap.c) ---- */

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

typedef float (*tonemap_fn)(float, float);

/* ---- readback quantization: float HDR buffer -> b-bit normalized -> float ---- */

static float quantize_b(float x, int b, float xmax) {
    if (x <= 0.0f) return 0.0f;
    if (x >= xmax) return xmax;                 /* HDR tail clipped by readback */
    double levels = (double)((1ULL << b) - 1);
    long q = lround((double)x / (double)xmax * levels); /* GPU integer store */
    return (float)((double)q / levels * (double)xmax);
}

/* ---- scenes: three HDR brightness tiers, deterministic ---- */

static void make_scene(int tier, float* img, float* xmax_out) {
    /* tier 0 dark / 1 mid / 2 bright: scale + offset, plus a highlight ramp
       that pushes real HDR values past the readback headroom in tiers 1-2 */
    const float base[3]  = {0.02f, 0.25f, 1.20f};
    const float span[3]  = {0.30f, 2.50f, 8.00f};
    float xmax = 0.0f;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float u = (float)x / (W - 1);          /* horizontal luminance ramp */
            float v = (float)y / (H - 1);
            float r = base[tier] + span[tier] * u;
            /* sparse bright specular spots: up to ~3x ramp head */
            int sx = ((x * 7 + y * 13) % 64 == 0);
            if (sx) r *= 3.0f;
            /* gentle vertical variation so max-diff coords are not degenerate */
            r *= 0.95f + 0.10f * v;
            img[y * W + x] = r;
            if (r > xmax) xmax = r;
        }
    }
    *xmax_out = xmax;
}

int main(void) {
    const char* tier_name[NLEVELS] = {"dark", "mid", "bright"};
    const char* curve_name[2] = {"reinhard", "aces"};
    const float exposure = 1.0f;
    tonemap_fn curves[2] = {reinhard, aces};

    FILE* summary = fopen("analysis/out/summary.csv", "w");
    FILE* detail  = fopen("analysis/out/depth_sweep.csv", "w");
    if (!summary || !detail) { perror("fopen"); return 1; }
    fprintf(summary, "tier,curve,depth,max_abs_diff,mean_abs_diff,worst_x,worst_y,within_one_level\n");
    fprintf(detail, "tier,curve,depth,max_abs_diff,mean_abs_diff,within_one_level\n");

    int overall_fail = 0;

    for (int tier = 0; tier < NLEVELS; tier++) {
        float* hdr = malloc(sizeof(float) * NPIX);
        float xmax;
        make_scene(tier, hdr, &xmax);

        for (int ci = 0; ci < 2; ci++) {
            tonemap_fn T = curves[ci];

            /* export reference: full precision, once per scene/curve */
            float* ref = malloc(sizeof(float) * NPIX);
            for (int i = 0; i < NPIX; i++) ref[i] = T(hdr[i], exposure);

            for (int di = 0; di < NDEPTHS; di++) {
                int b = depths[di];
                float maxdiff = 0.0f, sumdiff = 0.0f;
                int worst = 0;
                for (int i = 0; i < NPIX; i++) {
                    float q = quantize_b(hdr[i], b, xmax);
                    float p = T(q, exposure);
                    float d = fabsf(p - ref[i]);
                    sumdiff += d;
                    if (d > maxdiff) { maxdiff = d; worst = i; }
                }
                int ok = maxdiff <= TOL;
                fprintf(summary, "%s,%s,%d,%.8f,%.8f,%d,%d,%d\n",
                        tier_name[tier], curve_name[ci], b, maxdiff,
                        sumdiff / NPIX, worst % W, worst / W, ok);
                fprintf(detail, "%s,%s,%d,%.8f,%.8f,%d\n",
                        tier_name[tier], curve_name[ci], b, maxdiff,
                        sumdiff / NPIX, ok);
                if (b == 8 && !ok) overall_fail++;
            }
            free(ref);
        }
        free(hdr);
    }
    fclose(summary);
    fclose(detail);
    printf("tol=1/255=%.8f  scenes=%d curves=2 depths=%d  b8_fail_cases=%d\n",
           TOL, NLEVELS, NDEPTHS, overall_fail);
    return 0;
}
