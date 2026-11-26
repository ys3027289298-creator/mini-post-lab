/*
 * lut_bench: sweep the spacing-law exponent p and emit CSV.
 *
 * Columns:
 *   p, h_dark, h_bright, spacing_ratio,
 *   dL_tier0 (L=0.05), dL_tier1 (0.18), dL_tier2 (0.50),
 *   ns_lookup (min of 3 timed runs, generic sampler)
 *
 * The C test owns the go/no-go checks; this program only produces numbers
 * for the analysis script and the trade-off charts.
 */
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "lut.h"

#define N_NODES 17
#define ITERS 2000000

static float lab_l(float y) {
    const float e = 0.008856f, k = 903.3f;
    if (y <= 0.0f) return 0.0f;
    return y > e ? 116.0f * cbrtf(y) - 16.0f : k * y;
}

static float tier_error(const Lut3d* lut, float center, float half_width) {
    float worst = 0.0f;
    enum { STEPS = 401 };
    float lo = center - half_width < 0.0f ? 0.0f : center - half_width;
    float hi = center + half_width > 1.0f ? 1.0f : center + half_width;
    for (int s = 0; s <= STEPS; s++) {
        float x = lo + (hi - lo) * (float)s / (float)STEPS;
        float in[3] = { x, x, x }, got[3], want[3];
        lut_sample(lut, in, got);
        lut_reference(in, want);
        float d = fabsf(lab_l(got[0]) - lab_l(want[0]));
        if (d > worst) worst = d;
    }
    return worst;
}

static double time_lookup(const Lut3d* lut) {
    static float sink[3];
    struct timespec t0, t1;
    double best = 1e18;
    for (int rep = 0; rep < 3; rep++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (long i = 0; i < ITERS; i++) {
            float in[3] = { (float)(i & 255) / 255.0f, 0.3f, 0.7f };
            lut_sample(lut, in, sink);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e6;
        double ns = ms * 1e6 / ITERS;
        if (ns < best) best = ns;
    }
    return best;
}

int main(void) {
    static Lut3d lut;
    const float tiers[3] = { 0.05f, 0.18f, 0.50f };
    const float halfw = 0.02f;

    printf("p,h_dark,h_bright,ratio,dL_dark,dL_mid,dL_bright,ns_lookup\n");
    for (int pi = 0; pi <= 30; pi++) {
        float p = 1.0f + 0.1f * (float)pi;
        lut_set_nodes(&lut, N_NODES, p);
        lut_bake(&lut, (LutTransform)lut_reference);

        float h_dark = lut.nodes[1] - lut.nodes[0];
        float h_bright = lut.nodes[N_NODES - 1] - lut.nodes[N_NODES - 2];
        float e0 = tier_error(&lut, tiers[0], halfw);
        float e1 = tier_error(&lut, tiers[1], halfw);
        float e2 = tier_error(&lut, tiers[2], halfw);
        double ns = time_lookup(&lut);

        printf("%.2f,%.8f,%.8f,%.6f,%.4f,%.4f,%.4f,%.2f\n",
               p, h_dark, h_bright, h_dark / h_bright, e0, e1, e2, ns);
    }
    return 0;
}
