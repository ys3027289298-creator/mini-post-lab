
/* color_audit: gamma-space vs linear-space blending reference kernel.
 *
 * All buffers are per-channel float. Two conventions coexist on purpose:
 *   - "linear" buffers hold radiance L in [0, +inf)
 *   - "encoded" buffers hold gamma-encoded values x = L^(1/g) in [0, 1]
 * The incident under audit: box blur / additive post-processing (see bloom/)
 * was applied to encoded buffers with linear arithmetic, so each machine's
 * local gamma turned one shared operation into different math.
 *
 * Build: make ; self-test: make test
 * CLI:   ./color_audit <gammaA> <gammaB> <mode: lin|gam|mix>  (debug aid)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static float encode_linear(float L, float g) { return powf(L, 1.0f / g); }
static float decode_encoded(float x, float g) { return powf(x, g); }

/* correct linear-space blend: weights apply to radiance */
static float blend_linear(float La, float Lb, float w) {
    return (1.0f - w) * La + w * Lb;
}

/* the bug: same weighted average, but taken on encoded values.
 * Returned in linear space so the two pipelines are comparable. */
static float blend_gamma(float La, float Lb, float w, float g) {
    float xa = encode_linear(La, g);
    float xb = encode_linear(Lb, g);
    return decode_encoded((1.0f - w) * xa + w * xb, g);
}

/* separable box blur on a linear buffer (energy preserving, cf. bloom.c) */
static int box_blur_linear(const float* src, float* dst, int n, int r) {
    if (n <= 0 || r < 0 || r * 2 + 1 > n) return -1;
    for (int i = 0; i < n; i++) {
        float s = 0.0f;
        for (int k = -r; k <= r; k++) {
            int j = i + k;
            if (j < 0) j = 0;
            if (j >= n) j = n - 1;
            s += src[j];
        }
        dst[i] = s / (float)(2 * r + 1);
    }
    return 0;
}

/* the buggy path: blur encoded values, hand the result back as linear.
 * tmp is caller-provided scratch of n floats. */
static int box_blur_gamma(const float* src_linear, float* dst_linear,
                          float* tmp, int n, int r, float g) {
    if (n <= 0 || r < 0 || r * 2 + 1 > n) return -1;
    for (int i = 0; i < n; i++) tmp[i] = encode_linear(src_linear[i], g);
    for (int i = 0; i < n; i++) {
        float s = 0.0f;
        for (int k = -r; k <= r; k++) {
            int j = i + k;
            if (j < 0) j = 0;
            if (j >= n) j = n - 1;
            s += tmp[j];
        }
        dst_linear[i] = decode_encoded(s / (float)(2 * r + 1), g);
    }
    return 0;
}

/* one machine: a machine is fully described by its display gamma and the
 * space in which it runs the shared post-processing op. */
typedef struct { float gamma; int linear_pipeline; } Machine;

static int run_machine(const Machine* m, const float* src, float* dst,
                       float* scratch, int n, int r) {
    if (m->linear_pipeline)
        return box_blur_linear(src, dst, n, r);
    return box_blur_gamma(src, dst, scratch, n, r, m->gamma);
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    /* --- CLI mode: print one blend through both pipelines -------------- */
    if (argc == 4) {
        float gA = strtof(argv[1], NULL), gB = strtof(argv[2], NULL);
        float La = 0.0f, Lb = 1.0f, w = 0.5f;
        if (strcmp(argv[3], "lin") == 0) {
            float y = blend_linear(La, Lb, w);
            printf("lin %.6f encA=%.6f encB=%.6f\n", y,
                   encode_linear(y, gA), encode_linear(y, gB));
            return 0;
        } else if (strcmp(argv[3], "gam") == 0) {
            float ya = blend_gamma(La, Lb, w, gA);
            float yb = blend_gamma(La, Lb, w, gB);
            printf("gam outA=%.6f encA=%.6f | outB=%.6f encB=%.6f | code_diff=%.3f\n",
                   ya, encode_linear(ya, gA), yb, encode_linear(yb, gB),
                   fabsf(encode_linear(ya, gA) - encode_linear(yb, gB)) * 255.0f);
            return 0;
        }
        fprintf(stderr, "usage: %s <gammaA> <gammaB> lin|gam\n", argv[0]);
        return 2;
    }

    /* --- self-tests ---------------------------------------------------- */
    /* encode/decode round trip */
    check("roundtrip", fabsf(decode_encoded(encode_linear(0.37f, 2.2f), 2.2f) - 0.37f) < 1e-6f);

    /* endpoints are space-invariant: black and white blend identically */
    check("white_invariant", fabsf(blend_gamma(1.0f, 1.0f, 0.5f, 2.2f) -
                                   blend_linear(1.0f, 1.0f, 0.5f)) < 1e-6f);
    check("black_invariant", fabsf(blend_gamma(0.0f, 0.0f, 0.5f, 2.2f) -
                                   blend_linear(0.0f, 0.0f, 0.5f)) < 1e-6f);

    /* black/white 50-50: linear gives 0.5, gamma path gives 0.5^g = 0.2176 (g=2.2) */
    float lb = blend_linear(0.0f, 1.0f, 0.5f);
    float gb = blend_gamma(0.0f, 1.0f, 0.5f, 2.2f);
    check("bw_linear_half", fabsf(lb - 0.5f) < 1e-6f);
    check("bw_gamma_darker", gb < lb);
    check("bw_gamma_value", fabsf(gb - powf(0.5f, 2.2f)) < 1e-5f);

    /* gamma path is always darker for interior values (Jensen gap, x^g convex) */
    float interior = blend_gamma(0.1f, 0.9f, 0.5f, 2.2f);
    check("gamma_convex_darker", interior < blend_linear(0.1f, 0.9f, 0.5f));

    /* flat field: both blurs preserve the value */
    float flat[8], out[8], scratch[8];
    for (int i = 0; i < 8; i++) flat[i] = 0.6f;
    box_blur_linear(flat, out, 8, 2);
    check("blur_lin_flat", fabsf(out[4] - 0.6f) < 1e-6f);
    box_blur_gamma(flat, out, scratch, 8, 2, 2.2f);
    check("blur_gam_flat", fabsf(out[4] - 0.6f) < 1e-6f);

    /* edge case guards */
    check("blur_bad_radius", box_blur_linear(flat, out, 8, 10) == -1);
    check("blur_bad_n", box_blur_linear(flat, out, 0, 1) == -1);

    /* two machines, same source, differing gamma on the buggy path */
    float spike[8]; memset(spike, 0, sizeof(spike)); spike[4] = 1.0f;
    float out_a[8], out_b[8];
    Machine art = { 1.8f, 0 }, prog = { 2.4f, 0 };
    run_machine(&art, spike, out_a, scratch, 8, 1);
    run_machine(&prog, spike, out_b, scratch, 8, 1);
    check("machines_disagree", fabsf(out_a[4] - out_b[4]) > 1e-3f);
    /* lower gamma -> lighter output on the same encoded average */
    check("low_gamma_lighter", out_a[4] > out_b[4]);
    /* two correct linear machines agree regardless of gamma */
    Machine lin1 = { 1.8f, 1 }, lin2 = { 2.4f, 1 };
    run_machine(&lin1, spike, out_a, scratch, 8, 1);
    run_machine(&lin2, spike, out_b, scratch, 8, 1);
    check("linear_machines_agree", fabsf(out_a[4] - out_b[4]) < 1e-6f);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
