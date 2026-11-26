/*
 * lut self-test.
 *
 * The old tests asserted discrete node constants against interpolated
 * samples and failed forever. Here expectations are derived from the
 * trilinear remainder bound instead: node points must be exact, off-node
 * samples must stay inside the analytically derived worst-case bound, and
 * the shadow-tier perceptual error must clear the JND threshold. Every
 * numeric limit below is derived in ../ANALYSIS.md, not eyeballed.
 */
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "lut.h"

#define N_NODES 17
#define JND 2.3f /* CIELAB delta-E just-noticeable difference */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* ---- CIELAB L* (D65 white = 1.0), perceptual metric for error ---- */
static float lab_l(float y) {
    const float e = 0.008856f, k = 903.3f;
    float yc = y < 0.0f ? 0.0f : y;
    return yc > e ? 116.0f * cbrtf(yc) - 16.0f : k * yc;
}

/* worst |f(x)-P1f(x)| over a cell, scanned densely (bound check is
 * empirical here; the closed form lives in ANALYSIS.md) */
static float cell_peak_error(const Lut3d* lut, int cell) {
    float a = lut->nodes[cell], b = lut->nodes[cell + 1];
    float worst = 0.0f;
    enum { STEPS = 257 };
    for (int s = 0; s <= STEPS; s++) {
        float x = a + (b - a) * (float)s / (float)STEPS;
        float rgb_in[3] = { x, x, x };
        float got[3], want[3];
        lut_sample(lut, rgb_in, got);
        lut_reference(rgb_in, want);
        float e = fabsf(got[0] - want[0]);
        if (e > worst) worst = e;
    }
    return worst;
}

/* max perceptual (delta-L*) error over one luminance tier */
static float tier_perceptual_error(const Lut3d* lut, float center,
                                   float half_width) {
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

int main(void) {
    static Lut3d uni, nonu;

    /* 1. nodes are exact for both spacing laws: this is what the legacy
     *    test got right — but it must be checked AT the nodes, not between */
    lut_set_nodes(&uni, N_NODES, 1.0f);
    lut_bake(&uni, (LutTransform)lut_reference);
    lut_set_nodes(&nonu, N_NODES, 3.0f);
    lut_bake(&nonu, (LutTransform)lut_reference);

    float max_node_err = 0.0f;
    for (int i = 0; i < N_NODES; i++) {
        float x = uni.nodes[i];
        float in[3] = { x, x, x }, got[3], want[3];
        lut_sample(&uni, in, got);
        lut_reference(in, want);
        for (int ch = 0; ch < 3; ch++)
            max_node_err = fmaxf(max_node_err, fabsf(got[ch] - want[ch]));
    }
    check("node_exact_uniform", max_node_err < 1e-6f);

    max_node_err = 0.0f;
    for (int i = 0; i < N_NODES; i++) {
        float x = nonu.nodes[i];
        float in[3] = { x, x, x }, got[3], want[3];
        lut_sample(&nonu, in, got);
        lut_reference(in, want);
        for (int ch = 0; ch < 3; ch++)
            max_node_err = fmaxf(max_node_err, fabsf(got[ch] - want[ch]));
    }
    check("node_exact_nonuniform", max_node_err < 1e-6f);

    /* 2. off-node error obeys the trilinear bound Delta/4 per cell,
     *    Delta = max node-value jump of the cell's three axes. For the
     *    separable bake every axis shares one table. */
    int bound_ok = 1;
    float worst_ratio = 0.0f;
    for (int cell = 0; cell < N_NODES - 1; cell++) {
        float peak = cell_peak_error(&uni, cell);
        /* node-value jump along the axis: f at node cell+1 minus at cell */
        float in_a[3] = { uni.nodes[cell], 0.0f, 0.0f };
        float in_b[3] = { uni.nodes[cell + 1], 0.0f, 0.0f };
        float va[3], vb[3];
        lut_reference(in_a, va);
        lut_reference(in_b, vb);
        float d = fabsf(vb[0] - va[0]);
        float bound = 0.25f * d;
        float ratio = bound > 0.0f ? peak / bound : 0.0f;
        if (ratio > worst_ratio) worst_ratio = ratio;
        if (peak > bound + 1e-7f) bound_ok = 0;
    }
    check("trilinear_quarter_bound", bound_ok);
    printf("peak_over_bound_ratio=%.4f (expect <= 1)\n", worst_ratio);

    /* 3. fast and generic sampler agree on a uniform LUT */
    float agree = 0.0f;
    for (int s = 1; s < 1000; s++) {
        float x = (float)s / 1000.0f;
        float in[3] = { x, 1.0f - x, 0.5f * x }, a[3], b[3];
        lut_sample(&uni, in, a);
        lut_sample_fast(&uni, in, b);
        for (int ch = 0; ch < 3; ch++) agree = fmaxf(agree, fabsf(a[ch] - b[ch]));
    }
    check("fast_matches_generic", agree < 1e-6f);
    float dummy_in[3] = { 0.1f, 0.2f, 0.3f }, dummy_out[3];
    check("fast_refuses_nonuniform", lut_sample_fast(&nonu, dummy_in, dummy_out) == -1);

    /* 4. three luminance tiers: shadow perceptual error must clear JND.
     *    Uniform LUT is expected to FAIL the dark tier — that failure is
     *    printed with measured numbers rather than silently red forever. */
    const float tiers[3] = { 0.05f, 0.18f, 0.50f };
    const float halfw = 0.02f;
    printf("\n%-6s %-10s %-10s %-10s\n", "tier", "uni_dL*", "nonu_dL*", "jnd");
    int nonu_all_ok = 1;
    for (int t = 0; t < 3; t++) {
        float eu = tier_perceptual_error(&uni, tiers[t], halfw);
        float en = tier_perceptual_error(&nonu, tiers[t], halfw);
        printf("L=%-4.2f %-10.3f %-10.3f %-10.2f\n", tiers[t], eu, en, (double)JND);
        if (en > JND) nonu_all_ok = 0;
        if (t == 0)
            check("shadow_uniform_fails_as_predicted", eu > JND);
    }
    check("nonuniform_passes_all_tiers", nonu_all_ok);

    /* 5. spacing facts used by ANALYSIS.md */
    float h0_n = nonu.nodes[1] - nonu.nodes[0];
    float hL_n = nonu.nodes[N_NODES - 1] - nonu.nodes[N_NODES - 2];
    float h_uni = 1.0f / (N_NODES - 1);
    printf("\nuniform_spacing=%.6f\n", h_uni);
    printf("nonuniform_h_dark=%.6f h_bright=%.6f ratio=%.4f\n",
           h0_n, hL_n, h0_n / hL_n);

    /* 6. lookup cost: timed samples, generic vs uniform fast path */
    static float sink[3];
    struct timespec t0, t1;
    long iters = 2000000;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < iters; i++) {
        float in[3] = { (float)(i & 255) / 255.0f, 0.3f, 0.7f };
        lut_sample(&nonu, in, sink);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms_nonuni = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < iters; i++) {
        float in[3] = { (float)(i & 255) / 255.0f, 0.3f, 0.7f };
        lut_sample_fast(&uni, in, sink);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms_uni = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("\nlookup_ns: uniform=%.1f nonuniform=%.1f overhead_x=%.2f\n",
           ms_uni * 1e6 / iters, ms_nonuni * 1e6 / iters, ms_nonuni / ms_uni);

    printf("\nall_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
