/*
 * analytic_check: derive minimum readback bit depth from the
 * "within one final quantization level" bound, then compare with the
 * measured depth sweep. Also dumps failing-pixel evidence for b=8.
 *
 * Interior (no clipping), per pixel:
 *   |T(q_b(x)) - T(x)| <= M * |q_b(x) - x| <= M * xmax / (2*(2^b - 1))
 * where M = max T'(v) over the scene's post-exposure range.
 * Require <= TOL = 1/255:
 *   2^b - 1 >= M * xmax / (2*TOL)
 *   b >= 1 + log2(1 + M*xmax/(2*TOL))   (ceiling, conservative envelope)
 *
 * Clipping tail (x > xmax): error = T(x) - T(xmax), reported separately;
 * no interior bit depth can remove it, which is why bright-tier b=8
 * worst pixels sit on clipped/specular samples.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#define W 256
#define H 256
#define NPIX (W * H)
#define TOL 0.00392156862745

static float reinhard(float x) { float v = x; return v / (1.0f + v); }
static float aces(float v) {
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float r = (v * (a * v + b)) / (v * (c * v + d) + e);
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

static float reinhard_d(float v) { return 1.0f / ((1.0f + v) * (1.0f + v)); }

static float aces_d(float v) {
    /* symbolic derivative of N/D with N=v*(a v+b), D=v*(c v+d)+e */
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float N  = v * (a * v + b);
    float D  = v * (c * v + d) + e;
    float Np = 2 * a * v + b;
    float Dp = 2 * c * v + d;
    return (Np * D - N * Dp) / (D * D);
}

static void make_scene(int tier, float* img, float* xmax_out) {
    const float base[3] = {0.02f, 0.25f, 1.20f};
    const float span[3] = {0.30f, 2.50f, 8.00f};
    float xmax = 0.0f;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = (float)x / (W - 1), v = (float)y / (H - 1);
            float r = base[tier] + span[tier] * u;
            if ((x * 7 + y * 13) % 64 == 0) r *= 3.0f;
            r *= 0.95f + 0.10f * v;
            img[y * W + x] = r;
            if (r > xmax) xmax = r;
        }
    *xmax_out = xmax;
}

static float quantize_b(float x, int b, float xmax) {
    if (x <= 0.0f) return 0.0f;
    if (x >= xmax) return xmax;
    double levels = (double)((1ULL << b) - 1);
    long q = lround((double)x / (double)xmax * levels);
    return (float)((double)q / levels * (double)xmax);
}

int main(void) {
    const char* tier_name[3] = {"dark", "mid", "bright"};
    const char* curve_name[2] = {"reinhard", "aces"};
    float (*T[2])(float) = {reinhard, aces};
    float (*dT[2])(float) = {reinhard_d, aces_d};

    FILE* f = fopen("analysis/out/analytic.csv", "w");
    fprintf(f, "tier,curve,xmax,M_slope,b_predicted_envelope,b_measured_min\n");

    for (int tier = 0; tier < 3; tier++) {
        float* hdr = malloc(sizeof(float) * NPIX);
        float xmax;
        make_scene(tier, hdr, &xmax);

        for (int ci = 0; ci < 2; ci++) {
            /* M: max slope actually reached by this scene */
            float M = 0.0f;
            for (int i = 0; i < NPIX; i++) {
                float s = fabsf(dT[ci](hdr[i]));
                if (s > M) M = s;
            }
            /* envelope inverse */
            double b_real = 1.0 + log2(1.0 + (double)M * xmax / (2.0 * TOL));
            int b_env = (int)ceil(b_real - 1e-9);

            /* measured minimum depth: sweep b=8..16 on this scene/curve */
            int b_meas = -1;
            for (int b = 8; b <= 16; b++) {
                float worst = 0.0f;
                for (int i = 0; i < NPIX; i++) {
                    float q = quantize_b(hdr[i], b, xmax);
                    float d = fabsf(T[ci](q) - T[ci](hdr[i]));
                    if (d > worst) worst = d;
                }
                if (worst <= TOL) { b_meas = b; break; }
            }
            printf("%-7s %-9s xmax=%7.3f M=%6.3f b_real=%5.2f b_envelope=%2d b_measured=%2d\n",
                   tier_name[tier], curve_name[ci], xmax, M, b_real, b_env, b_meas);
            fprintf(f, "%s,%s,%.6f,%.6f,%d,%d\n",
                    tier_name[tier], curve_name[ci], xmax, M, b_env, b_meas);
        }
        free(hdr);
    }
    fclose(f);

    /* failing-pixel evidence at b=8 for the two failing tiers, both curves */
    FILE* g = fopen("analysis/out/fail_pixels_b8.csv", "w");
    fprintf(g, "tier,curve,worst_x,worst_y,hdr_value,quantized_value,export_L,preview_L,abs_diff,levels_over_tol,clipped\n");
    for (int tier = 1; tier <= 2; tier++) {
        float* hdr = malloc(sizeof(float) * NPIX);
        float xmax;
        make_scene(tier, hdr, &xmax);
        for (int ci = 0; ci < 2; ci++) {
            float worst = 0.0f; int wi = 0;
            for (int i = 0; i < NPIX; i++) {
                float q = quantize_b(hdr[i], 8, xmax);
                float d = fabsf(T[ci](q) - T[ci](hdr[i]));
                if (d > worst) { worst = d; wi = i; }
            }
            float q = quantize_b(hdr[wi], 8, xmax);
            float L = T[ci](hdr[wi]), P = T[ci](q);
            fprintf(g, "%s,%s,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.2f,%d\n",
                    tier_name[tier], curve_name[ci], wi % W, wi / W,
                    hdr[wi], q, L, P, fabsf(P - L), fabsf(P - L) / TOL,
                    hdr[wi] >= xmax);
        }
        free(hdr);
    }
    fclose(g);
    return 0;
}
