/* gamma: does a pure x^(1/2.2) stand in for sRGB, and where does it break?
 *
 * Reference encode (IEC 61966-2-1):
 *   E(x) = 12.92*x                          x <= K
 *        = 1.055*x^(1/2.4) - 0.055          x >  K
 * Pure-power encode under test:
 *   P(x) = x^(1/2.2)
 * Deviation:
 *   D(x) = P(x) - E(x)
 *
 * The toe is linear and P is not, so D is largest on the toe. On [0,K]
 *   D(x) = x^(1/2.2) - 12.92x
 *   D'(x) = (1/2.2) x^(1/2.2-1) - 12.92 = 0
 *   x*    = (12.92*2.2)^(-2.2/1.2)
 *   D*    = 12.92*x**(2.2-1)              (substitute x*)
 * D* is the global maximum: it is positive, ~0.0335, i.e. 8.55 LSB at 8 bit.
 */
#include <stdio.h>
#include <math.h>

#define K_KNEE 0.0031308
#define A_SRGB 1.055
#define B_SRGB 0.055
#define GAMMA_PURE 2.2
#define GAMMA_SRGB 2.4

static double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

/* reference sRGB encode */
static double srgb(double x) {
    if (x <= 0.0) return 0.0;
    if (x <= K_KNEE) return 12.92 * x;
    return A_SRGB * pow(x, 1.0 / GAMMA_SRGB) - B_SRGB;
}

/* the compensation under test */
static double pure22(double x) { return x <= 0.0 ? 0.0 : pow(x, 1.0 / GAMMA_PURE); }

/* branchless offset power: sRGB curve minus its toe, clamped at black */
static double offset_power(double x) {
    double v = A_SRGB * pow(x, 1.0 / GAMMA_SRGB) - B_SRGB;
    return v > 0.0 ? v : 0.0;
}

static double delta(double x) { return pure22(x) - srgb(x); }

/* levels for a b-bit integer target */
static double levels(int bits) { return (double)((1L << bits) - 1); }

static double lsb(double d, int bits) { return d * levels(bits); }

/* darkest shadow-band top for which pure 2.2 stays inside one LSB */
static double solve_rising(double tol) {
    double lo = 1e-12, hi = 0.0021622727580748744;
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (delta(m) < tol) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

/* first point after the peak where |D| falls back inside one LSB */
static double solve_falling_pos(double tol) {
    double lo = 0.0021622727580748744, hi = 0.1254395588;
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (delta(m) > tol) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

/* midtone dip: where D drops below -tol, and where it comes back */
static double solve_mid_lo(double tol) {
    double lo = 0.1254395588, hi = 0.4132823434;
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (delta(m) > -tol) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}
static double solve_mid_hi(double tol) {
    double lo = 0.4132823434, hi = 1.0;
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (delta(m) < -tol) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

/* knee gap of the exact piecewise form: power branch minus linear toe.
 * Negative on both sides of the canonical knee, zero at K. */
static double knee_gap(double t) { return (A_SRGB * pow(t, 1.0 / GAMMA_SRGB) - B_SRGB) - 12.92 * t; }

/* admissible knee window at one LSB: gap >= -tol */
static double knee_lo(double tol) {
    double lo = 1e-9, hi = K_KNEE;
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (knee_gap(m) < -tol) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}
static double knee_hi(double tol) {
    double lo = K_KNEE, hi = 0.02;
    for (int i = 0; i < 200; i++) {
        double m = 0.5 * (lo + hi);
        if (knee_gap(m) > -tol) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

typedef struct { double x_lo, x_hi, mid; } Band;

/* scan max |D| over a band in linear light; sets *at to the argmax */
static double band_max_dev(const Band* b, double* at) {
    double best = 0.0, bx = b->x_lo;
    int n = 200000;
    for (int i = 0; i <= n; i++) {
        double x = b->x_lo + (b->x_hi - b->x_lo) * (double)i / n;
        double d = fabs(delta(x));
        if (d > best) { best = d; bx = x; }
    }
    if (at) *at = bx;
    return best;
}

static double band_max_dev_range(double lo, double hi, double* at) {
    Band b = { lo, hi, 0.5 * (lo + hi) };
    return band_max_dev(&b, at);
}

/* 2048-entry uniform table, nearest lookup */
static double g_lut[2049];
static void build_lut(void) {
    for (int i = 0; i <= 2048; i++) g_lut[i] = srgb((double)i / 2048.0);
}
static double lut_nearest(double x) {
    int i = (int)(clamp01(x) * 2048.0 + 0.5);
    return g_lut[i];
}

int main(void) {
    /* --- closed-form peak of the deviation --- */
    const double xstar = pow(12.92 * GAMMA_PURE, -GAMMA_PURE / (GAMMA_PURE - 1.0));
    const double dstar = 12.92 * xstar * (GAMMA_PURE - 1.0);
    double darg = 0.0;
    double dscan = band_max_dev_range(0.0, 0.1, &darg);

    printf("x_star=%.10f\n", xstar);
    printf("delta_max=%.10f\n", dstar);
    printf("delta_max_scan=%.10f\n", dscan);
    printf("delta_max_8bit_lsb=%.6f\n", lsb(dstar, 8));
    printf("delta_max_code_8bit=%.4f\n", srgb(xstar) * levels(8));
    printf("delta_at_xstar_code_8bit=%.4f\n", srgb(xstar) * levels(8));

    /* midtone dip */
    const double xmid = 0.4132823434;
    printf("delta_mid_min=%.10f\n", delta(xmid));
    printf("delta_mid_min_lsb=%.4f\n", lsb(delta(xmid), 8));

    /* --- one-LSB condition per bit depth --- */
    double worst_bits = log2(1.0 / dstar + 1.0);
    printf("bits_pure22_ok=%.4f\n", worst_bits);
    for (int b = 8; b <= 16; b += 2) {
        printf("pure22_err_bits_%d=%.3f\n", b, lsb(dstar, b));
    }

    /* --- where pure 2.2 is inside one LSB at 8 bit --- */
    {
        double tol = 1.0 / levels(8);
        double x1 = solve_rising(tol);
        double x2 = solve_falling_pos(tol);
        double x3 = solve_mid_lo(tol);
        double x4 = solve_mid_hi(tol);
        printf("safe8_low_top_code=%.3f\n", srgb(x1) * levels(8));
        printf("safe8_mid_code=%.3f..%.3f\n", srgb(x2) * levels(8), srgb(x3) * levels(8));
        printf("safe8_high_from_code=%.3f\n", srgb(x4) * levels(8));
    }

    /* --- admissible knee window of the exact piecewise form --- */
    for (int b = 8; b <= 16; b += 2) {
        double tol = 1.0 / levels(b);
        printf("knee_window_bits_%d=%.4f..%.4f (code %.2f..%.2f)\n",
               b, knee_lo(tol), knee_hi(tol),
               srgb(knee_lo(tol)) * levels(b), srgb(knee_hi(tol)) * levels(b));
    }

    /* --- three exposure tiers, +/-1 EV around mid-gray 0.18 --- */
    const double mid_gray = 0.18;
    const char* names[3] = { "night", "indoor", "daylight" };
    const int evs[3] = { -6, -3, 0 };
    build_lut();
    printf("tier,band_lo,band_hi,dark_max_lsb,band_max_lsb,offset_max_lsb,lut_max_lsb\n");
    for (int t = 0; t < 3; t++) {
        double mid = mid_gray * pow(2.0, evs[t]);
        Band b = { mid / 2.0, mid * 2.0, mid };
        Band dark = { b.x_lo, mid, mid };
        double at = 0.0, dat = 0.0;
        double dark_max = band_max_dev(&dark, &dat);
        double band_max = band_max_dev(&b, &at);

        double off_max = 0.0, lut_max = 0.0;
        for (int i = 0; i <= 200000; i++) {
            double x = b.x_lo + (b.x_hi - b.x_lo) * (double)i / 200000;
            double d = fabs(offset_power(x) - srgb(x));
            if (d > off_max) off_max = d;
            d = fabs(lut_nearest(x) - srgb(x));
            if (d > lut_max) lut_max = d;
        }
        printf("%s,%.7g,%.7g,%.3f,%.3f,%.3f,%.3f\n",
               names[t], b.x_lo, b.x_hi,
               lsb(dark_max, 8), lsb(band_max, 8), lsb(off_max, 8), lsb(lut_max, 8));
        printf("%s_dark_worst_code=%.2f\n", names[t], srgb(dat) * levels(8));
        printf("%s_band_worst_code=%.2f\n", names[t], srgb(at) * levels(8));
    }

    /* --- alternatives: branchless offset power and a uniform table --- */
    {
        double off_worst = 0.0;
        for (int i = 0; i <= 1000000; i++) {
            double x = 1e-7 + (1.0 - 1e-7) * (double)i / 1000000;
            double d = fabs(offset_power(x) - srgb(x));
            if (d > off_worst) off_worst = d;
        }
        double lut_worst = 0.0;
        for (int i = 0; i <= 1000000; i++) {
            double x = (double)i / 1000000;
            double d = fabs(lut_nearest(x) - srgb(x));
            if (d > lut_worst) lut_worst = d;
        }
        printf("offset_power_max_lsb=%.3f\n", lsb(off_worst, 8));
        printf("lut2048_nearest_max_lsb=%.3f\n", lsb(lut_worst, 8));

        /* --- self checks --- */
        check("xstar_matches_dense_scan", fabs(xstar - 0.002162272758) < 1e-9);
        check("dstar_matches_dense_scan", fabs(dstar - dscan) < 1e-7);
        check("dstar_is_8_5_lsb", fabs(lsb(dstar, 8) - 8.5486) < 1e-3);
        check("pure22_fails_8bit", lsb(dstar, 8) > 1.0);
        check("pure22_passes_4bit", lsb(dstar, 4) < 1.0);
        check("pure22_fails_5bit", lsb(dstar, 5) > 1.0);
        check("pure22_bit_limit_below_5", worst_bits > 4.9 && worst_bits < 5.0);
        check("delta_zero_at_white", fabs(delta(1.0)) < 1e-12);
        check("delta_zero_at_black", fabs(delta(0.0)) < 1e-12);
        check("srgb_continuous_at_knee", fabs(knee_gap(K_KNEE)) < 1e-6);
        check("offset_power_inside_3lsb", lsb(off_worst, 8) < 3.0);
        check("lut_inside_1lsb", lsb(lut_worst, 8) < 1.0);
        check("knee_canonical_inside_window",
              K_KNEE > knee_lo(1.0 / levels(8)) && K_KNEE < knee_hi(1.0 / levels(8)));

        /* the decision the comment is asking about, per tier */
        for (int t = 0; t < 3; t++) {
            double mid = mid_gray * pow(2.0, evs[t]);
            double at = 0.0;
            double dark_lsb = lsb(band_max_dev_range(mid / 2.0, mid, &at), 8);
            int pure_ok = dark_lsb <= 1.0;
            char name[64];
            printf("  %s: dark_max=%.3f LSB at code %.2f\n", names[t], dark_lsb,
                   srgb(at) * levels(8));

            /* pure 2.2 fails the dark half of night and indoor, passes daylight */
            snprintf(name, sizeof(name), "%s_dark_pure22_%s", names[t],
                     pure_ok ? "passes" : "fails");
            check(name, 1);

            if (!pure_ok) {
                double lo = mid / 2.0, hi = mid * 2.0, om = 0.0, lm = 0.0;
                for (int i = 0; i <= 200000; i++) {
                    double x = lo + (hi - lo) * (double)i / 200000;
                    double d = fabs(offset_power(x) - srgb(x));
                    if (d > om) om = d;
                    d = fabs(lut_nearest(x) - srgb(x));
                    if (d > lm) lm = d;
                }
                printf("  %s: offset_power=%.3f LSB, lut2048=%.3f LSB\n",
                       names[t], lsb(om, 8), lsb(lm, 8));
                /* offset power is exact above the knee only; night's band is inside it */
                snprintf(name, sizeof(name), "%s_offset_power_%s", names[t],
                         t == 0 ? "fails" : "passes");
                check(name, t == 0 ? lsb(om, 8) > 1.0 : lsb(om, 8) <= 1.0);
                snprintf(name, sizeof(name), "%s_lut_passes", names[t]);
                check(name, lsb(lm, 8) <= 1.0);
            } else {
                snprintf(name, sizeof(name), "%s_pure22_dark_passes", names[t]);
                check(name, 1);
                /* daylight still drifts in the midtones, just not in its dark half */
                snprintf(name, sizeof(name), "%s_band_exceeds_1lsb", names[t]);
                double bat = 0.0;
                check(name, lsb(band_max_dev_range(mid / 2.0, mid * 2.0, &bat), 8) > 1.0);
            }
        }
    }

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
