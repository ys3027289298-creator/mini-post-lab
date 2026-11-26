/* srgb: storage space vs linear compute space audit.
 *
 * Storage values are sRGB encoded c in [0,1].  Compute buffers are linear
 * light l.  IEC 61966-2-1 gives:
 *
 *   decode(c) = c / 12.92                         c <= 0.04045
 *             = ((c + 0.055) / 1.055)^2.4          c >  0.04045
 *
 *   encode(l) = 12.92 l                           l <= 0.0031308
 *             = 1.055 l^(1/2.4) - 0.055           l >  0.0031308
 *
 * Feeding c to a linear buffer without decode uses c where decode(c) is
 * required.  The normalized linear-light error is therefore
 *
 *   E(c) = c - decode(c).
 *
 * It is non-negative on [0,1]: misuse lifts shadows/midtones.  Its largest
 * value is ~0.2871, 73.22 levels of an 8-bit encoding.  Near white the error
 * returns to zero, but the identity curve has much less slope than the sRGB
 * EOTF, so highlight gradation/headroom is compressed.
 *
 * A non-zero "fraction of assets that may skip decode" only makes sense for
 * an aggregate/mixture error.  With equal asset weights and all skipped
 * assets sitting at the worst brightness, E_fleet = (k/N) E*.  A per-asset
 * maximum does not shrink with k/N: one unrestricted skipped asset already
 * reaches E*.
 */
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define Q8 (1.0 / 255.0)
#define CODE_LEVELS 255.0
#define C_KNEE 0.04045
#define L_KNEE 0.0031308
#define A_SRGB 1.055
#define B_SRGB 0.055
#define TOE_GAIN 12.92
#define GAMMA 2.4

static int failures = 0;

static void check(const char *name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static double srgb_decode(double c) {
    if (c <= 0.0) return 0.0;
    if (c <= C_KNEE) return c / TOE_GAIN;
    return pow((c + B_SRGB) / A_SRGB, GAMMA);
}

static double srgb_encode(double l) {
    if (l <= 0.0) return 0.0;
    if (l <= L_KNEE) return TOE_GAIN * l;
    return A_SRGB * pow(l, 1.0 / GAMMA) - B_SRGB;
}

static double misuse_error(double c) { return c - srgb_decode(c); }

static int roundtrip_all_8bit(void) {
    for (int code = 0; code <= 255; code++) {
        double c = code / CODE_LEVELS;
        if (fabs(srgb_encode(srgb_decode(c)) - c) > 1e-12) return 0;
    }
    return 1;
}

/* Analytic maximum of E(c) on the power branch. */
static double analytic_peak(double *c_star, double *l_star) {
    double l = pow(A_SRGB / GAMMA, 12.0 / 7.0);
    double c = A_SRGB * pow(l, 1.0 / GAMMA) - B_SRGB;
    if (c_star) *c_star = c;
    if (l_star) *l_star = l;
    return c - l;
}

typedef struct {
    int code;
    double c, linear, error, lsb;
} CodeWorst;

static CodeWorst scan_8bit(void) {
    CodeWorst w = {0, 0.0, 0.0, 0.0, 0.0};
    for (int code = 0; code <= 255; code++) {
        double c = code / CODE_LEVELS;
        double l = srgb_decode(c);
        double e = c - l;
        if (e > w.error) {
            w.code = code;
            w.c = c;
            w.linear = l;
            w.error = e;
            w.lsb = e * CODE_LEVELS;
        }
    }
    return w;
}

/* High root of E(c) = tol; the low root is linear on the toe. */
static double high_safe_root(double tol) {
    double lo = 0.5, hi = 1.0;
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (misuse_error(mid) > tol) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static uint64_t rng_state = 0x123456789abcdef0ULL;
static uint32_t xorshift32(void) {
    uint32_t x = (uint32_t)(rng_state ^= rng_state >> 12);
    x ^= x << 13;
    x ^= x >> 16;
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return x;
}

static void run_bench(void) {
    enum { BYTES = 65536, ITERS = 20000000 };
    static unsigned char codes[BYTES];
    static double lut[256];
    for (int i = 0; i < BYTES; i++) codes[i] = (unsigned char)(xorshift32() & 255u);
    for (int i = 0; i < 256; i++) lut[i] = srgb_decode(i / CODE_LEVELS);

    double t0, t1, acc = 0.0;

    t0 = now_seconds();
    for (int i = 0; i < ITERS; i++) {
        double c = codes[i & (BYTES - 1)] / CODE_LEVELS;
        acc += c;
    }
    t1 = now_seconds();
    double identity_ns = 1e9 * (t1 - t0) / ITERS;

    t0 = now_seconds();
    for (int i = 0; i < ITERS; i++) {
        int code = codes[i & (BYTES - 1)];
        acc += srgb_decode(code / CODE_LEVELS);
    }
    t1 = now_seconds();
    double pow_ns = 1e9 * (t1 - t0) / ITERS;

    t0 = now_seconds();
    for (int i = 0; i < ITERS; i++) {
        int code = codes[i & (BYTES - 1)];
        acc += lut[code];
    }
    t1 = now_seconds();
    double lut_ns = 1e9 * (t1 - t0) / ITERS;

    printf("bench_samples=%d\n", ITERS);
    printf("bench_identity_ns_per_sample=%.4f\n", identity_ns);
    printf("bench_piecewise_pow_ns_per_sample=%.4f relative=%.2fx\n",
           pow_ns, pow_ns / identity_ns);
    printf("bench_lut_ns_per_sample=%.4f relative=%.2fx\n",
           lut_ns, lut_ns / identity_ns);
    printf("bench_checksum=%.6f\n", acc);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        run_bench();
        return 0;
    }

    double c_star = 0.0, l_star = 0.0;
    double e_star = analytic_peak(&c_star, &l_star);
    CodeWorst w = scan_8bit();

    double alpha_mix = Q8 / e_star;
    double c_safe_lo = Q8 / (1.0 - 1.0 / TOE_GAIN);
    double c_safe_hi = high_safe_root(Q8);
    double safe_width = c_safe_lo + (1.0 - c_safe_hi);

    int safe_codes[256], n_safe = 0;
    for (int code = 0; code <= 255; code++) {
        if (misuse_error(code / CODE_LEVELS) <= Q8 + 1e-12)
            safe_codes[n_safe++] = code;
    }

    printf("=== sRGB storage/linear compute mismatch ===\n");
    printf("analytic_peak_c=%.10f (code %.4f)\n", c_star, c_star * CODE_LEVELS);
    printf("analytic_peak_linear=%.10f\n", l_star);
    printf("analytic_delta_max=%.10f\n", e_star);
    printf("analytic_delta_max_8bit_lsb=%.6f\n", e_star * CODE_LEVELS);
    printf("discrete_worst_code=%d c=%.10f linear=%.10f delta_lsb=%.6f\n",
           w.code, w.c, w.linear, w.lsb);
    printf("aggregate_alpha_max=%.10f (%.4f%%)\n", alpha_mix, 100.0 * alpha_mix);
    printf("safe_continuous_c=[0,%.7f] U [%.7f,1]\n", c_safe_lo, c_safe_hi);
    printf("safe_continuous_width_percent=%.4f%%\n", 100.0 * safe_width);
    printf("safe_8bit_codes=%d", n_safe);
    for (int i = 0; i < n_safe; i++) printf("%s%d", i ? "," : "[", safe_codes[i]);
    printf("]\n\n");

    const int tiers[3] = {10, 100, 1000};
    const char *names[3] = {"small", "medium", "large"};

    printf("tier,n,case,undecoded_k,undecoded_ratio,"
           "aggregate_worst_lsb,per_asset_max_lsb,worst_code,worst_c,verdict\n");

    int budget_counts[3];
    for (int t = 0; t < 3; t++) {
        int n = tiers[t];
        int k_ok = (int)floor((double)n * alpha_mix + 1e-10);
        budget_counts[t] = k_ok;

        struct Row {
            const char *label;
            int k;
        } rows[3] = {
            {"budget_max", k_ok},
            {"first_integer_over", k_ok + 1},
            {"all_undecoded", n},
        };

        for (int r = 0; r < 3; r++) {
            int k = rows[r].k;
            double ratio = (double)k / (double)n;
            double aggregate_lsb = ratio * w.lsb;
            double per_asset_lsb = k > 0 ? w.lsb : 0.0;
            int aggregate_pass = aggregate_lsb <= 1.0 + 1e-9;
            int strict_max_pass = k == 0;
            const char *verdict = strict_max_pass ? "PASS_MAX"
                                : aggregate_pass ? "PASS_AGGREGATE_FAILS_MAX"
                                                 : "EXCEEDS";
            printf("%s,%d,%s,%d,%.6f,%.6f,%.6f,%d,%.6f,%s\n",
                   names[t], n, rows[r].label, k, ratio,
                   aggregate_lsb, per_asset_lsb, w.code, w.c, verdict);
        }

        /* The numbers the prompt asks to report for an over-budget tier. */
        int k_over = k_ok + 1;
        double ratio_over = (double)k_over / (double)n;
        double aggregate_over = ratio_over * w.lsb;
        printf("# %s: first over count=%d ratio=%.4f%% "
               "aggregate_max=%.4f LSB per_asset_max=%.4f LSB "
               "at code %d / c=%.4f / linear=%.4f\n",
               names[t], k_over, 100.0 * ratio_over, aggregate_over,
               w.lsb, w.code, w.c, w.linear);
    }

    printf("\n=== cost/accuracy curve (alpha = undecoded equal-weight fraction) ===\n");
    printf("alpha,max_error_lsb,pow_decode_calls_relative\n");
    double alphas[] = {0.0, alpha_mix, 0.01, 0.02, 0.10, 0.50, 1.0};
    for (unsigned i = 0; i < sizeof(alphas) / sizeof(alphas[0]); i++) {
        double a = alphas[i];
        printf("%.6f,%.4f,%.6f\n", a, a * w.lsb, 1.0 - a);
    }

    printf("\n=== self checks ===\n");
    check("decode_black", fabs(srgb_decode(0.0)) < 1e-15);
    check("decode_white", fabs(srgb_decode(1.0) - 1.0) < 1e-12);
    check("decode_code128_known",
          fabs(srgb_decode(128.0 / 255.0) - 0.2158605001) < 1e-7);
    check("roundtrip_all_8bit", roundtrip_all_8bit());
    check("knee_decode_continuous",
          fabs(pow((C_KNEE + B_SRGB) / A_SRGB, GAMMA) - C_KNEE / TOE_GAIN) < 1e-6);
    check("peak_matches_8bit_scan", fabs(e_star - w.error) < 5e-5);
    check("peak_is_about_73_2_lsb", fabs(e_star * CODE_LEVELS - 73.2197) < 1e-3);
    check("peak_near_mid_code_136", w.code == 136);
    check("aggregate_budget_about_1_37pct",
          alpha_mix > 0.01365 && alpha_mix < 0.01367);
    check("safe_8bit_codes_0_1_255",
          n_safe == 3 && safe_codes[0] == 0 && safe_codes[1] == 1 &&
          safe_codes[2] == 255);
    check("small_budget_zero_asset", budget_counts[0] == 0);
    check("medium_budget_one_asset", budget_counts[1] == 1);
    check("large_budget_thirteen_assets", budget_counts[2] == 13);

    for (int t = 0; t < 3; t++) {
        int n = tiers[t];
        int k_ok = budget_counts[t];
        double agg_ok = ((double)k_ok / n) * w.lsb;
        int k_over = k_ok + 1;
        double agg_over = ((double)k_over / n) * w.lsb;
        char name[128];

        snprintf(name, sizeof(name), "%s_budget_aggregate_within_1lsb", names[t]);
        check(name, agg_ok <= 1.0 + 1e-9);
        snprintf(name, sizeof(name), "%s_next_asset_exceeds_aggregate", names[t]);
        check(name, agg_over > 1.0 + 1e-9);
        snprintf(name, sizeof(name), "%s_any_skipped_fullrange_asset_fails_max", names[t]);
        check(name, w.lsb > 1.0);
    }

    check("full_decode_has_zero_mismatch", fabs(misuse_error(1.0) - 0.0) < 1e-12);
    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
