
/* histeq: normalize two histogram-equalization artifacts onto one scale, solve
 * the peak-height crossover, and pick global vs local under a cost budget.
 *
 * ============================ the two artifacts ============================
 * Work in output levels / bins (one level = one unit).  Probe histogram:
 * M = PROBE_SAMPLES counts spread over N = N_LEVELS levels; one level holds
 * `peak` counts, the other N-1 share the rest evenly.  Let pi = peak / M be the
 * mass fraction sitting on that single level.  pi is the natural knob: it is
 * small for a smooth histogram and approaches 1 for a clipped/saturated one.
 *
 * GLOBAL equalization (classic HE):  out(i) = (N-1) * CDF(i).
 *   adjacent-level output step = (N-1) * h(i) / M
 *   adjacent-level input  step = 1
 *   slope at level i          = (N-1) * h(i) / M                     [-]
 *   On the sparse shoulders h = (M - peak)/(N-1), so
 *       slope = (M - peak)/M = 1 - pi.
 *   The peak is stretched, the shoulders crushed from slope 1 to (1 - pi).
 *   Fractional contrast lost near the peak:
 *       D_g = 1 - slope = pi.                                      [fraction]
 *   (This is exactly the "adjacent output diff / original diff" ratio minus 1.)
 *
 * LOCAL equalization (windowed / CLAHE): preserves local contrast, but an edge
 *   inside a window gets an overshoot -- a halo -- when the local gain is
 *   integrated out over the halo radius.  The user's measure is
 *       D_l = R * C,   R = halo radius [levels], C = contrast boost [level/level].
 *   A window can only redistribute the dynamic range it actually contains.  When
 *   one level already holds fraction pi of the counts, the range left for the
 *   window to work with is (1 - pi); both the halo radius it can spend and the
 *   contrast boost it can apply are bounded by that same remaining range:
 *       R(pi) = R_MAX * (1 - pi),   C(pi) = C_MAX * (1 - pi).
 *   So   D_l(pi) = R_MAX*C_MAX * (1-pi)^2                       [levels].
 *
 * ============================ normalization ================================
 * The two D's carry different units (fraction vs levels), so each is divided by
 * its own maximum over the operating envelope pi in [0,1]:
 *       d_g(pi) = D_g / 1                = pi                 (max 1 at pi=1)
 *       d_l(pi) = D_l / (R_MAX*C_MAX)    = (1-pi)^2           (max 1 at pi=0)
 * Both now lie in [0,1] and are directly comparable: the unified distortion of
 * an algorithm is its normalized value, d_g or d_l.
 *
 * ============================ the crossover ================================
 * The switch point is where the normalized distortions are equal:
 *       pi = (1 - pi)^2   ->   pi^2 - 3*pi + 1 = 0
 *       pi* = (3 - sqrt(5))/2 = 1/phi^2 = 0.381966...   (phi = golden ratio)
 * In peak counts:   peak* = pi* * M.
 *   pi < pi*  ->  d_g < d_l  ->  global distorts less.
 *   pi > pi*  ->  d_l < d_g  ->  local distorts less but costs more.
 *
 * ============================ the decision =================================
 * For each tier evaluate both normalized distortions, then choose the
 * distortion-optimal algorithm among the cost-feasible ones.  A tier passes
 * only if the chosen algorithm's unified distortion <= U_MAX and its cost
 * <= BUDGET_MS.  Otherwise the tier is reported with peak, both normalized
 * distortions and the crossover position.
 */
#include <stdio.h>
#include <math.h>

/* ---- model constants -------------------------------------------------- */
#define N_LEVELS       256.0
#define PROBE_SAMPLES  16384.0
#define R_MAX          64.0     /* max halo radius, levels */
#define C_MAX          4.0      /* max contrast boost, level/level */
#define DISPLAY_MP     2.0736   /* 1920x1080, megapixels */
#define RATE_GLOBAL    0.55     /* ms per megapixel, one HE pass */
#define RATE_LOCAL     3.00     /* ms per megapixel, windowed pass */
#define BUDGET_MS      5.0      /* per-frame compute cap */
#define U_MAX          0.40     /* unified distortion target */
#define EPS            1e-12

typedef enum { ALG_GLOBAL = 0, ALG_LOCAL = 1 } Alg;

/* ---- model ----------------------------------------------------------- */

static double halo_radius(double pi)   { return R_MAX * (1.0 - pi); }
static double contrast_boost(double pi){ return C_MAX * (1.0 - pi); }

/* normalized distortions, both on [0,1] */
static double d_global(double pi) { return pi; }
static double d_local(double pi)  { return (1.0 - pi) * (1.0 - pi); }

/* peak fraction where d_g == d_l */
static double crossover_pi(void) { return (3.0 - sqrt(5.0)) / 2.0; }

static double cost_ms(Alg a) {
    return ((a == ALG_GLOBAL) ? RATE_GLOBAL : RATE_LOCAL) * DISPLAY_MP;
}
static const char* alg_name(Alg a) { return a == ALG_GLOBAL ? "global" : "local"; }

/* ---- per-tier evaluation --------------------------------------------- */

typedef struct {
    const char* name;
    double peak, pi, dg, dl;
    Alg    best;              /* distortion-optimal, cost ignored */
    Alg    chosen;            /* distortion-optimal among cost-feasible */
    int    feasible_choices;  /* how many algorithms fit the budget */
    double u;                 /* unified distortion of the chosen algorithm */
    int    pass;
} Tier;

static void eval_tier(Tier* t, const char* name, double peak) {
    t->name = name;
    t->peak = peak;
    t->pi   = peak / PROBE_SAMPLES;
    t->dg   = d_global(t->pi);
    t->dl   = d_local(t->pi);

    t->best = (t->dg <= t->dl) ? ALG_GLOBAL : ALG_LOCAL;

    int g_ok = cost_ms(ALG_GLOBAL) <= BUDGET_MS + EPS;
    int l_ok = cost_ms(ALG_LOCAL)  <= BUDGET_MS + EPS;
    t->feasible_choices = g_ok + l_ok;

    double dg_f = g_ok ? t->dg : INFINITY;
    double dl_f = l_ok ? t->dl : INFINITY;
    t->chosen = (dg_f <= dl_f) ? ALG_GLOBAL : ALG_LOCAL;
    t->u      = (t->chosen == ALG_GLOBAL) ? t->dg : t->dl;
    t->pass   = isfinite(t->u) && (t->u <= U_MAX + EPS);
}

/* ---- self-test ------------------------------------------------------- */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

int main(void) {
    const double pstar = crossover_pi();
    const double peakstar = pstar * PROBE_SAMPLES;

    /* crossover is the intersection of the two normalized curves */
    check("crossover_equal",     near(d_global(pstar), d_local(pstar), 1e-12));
    check("crossover_quadratic", near(pstar*pstar - 3.0*pstar + 1.0, 0.0, 1e-12));
    {
        const double phi = (1.0 + sqrt(5.0)) / 2.0;
        check("crossover_is_invphi2", near(pstar, 1.0/(phi*phi), 1e-12));
    }

    /* ordering flips across the threshold */
    check("below_global_wins", d_global(pstar - 0.05) < d_local(pstar - 0.05));
    check("above_local_wins",  d_local(pstar + 0.05) < d_global(pstar + 0.05));

    /* both normalized to [0,1] and monotone */
    check("dg_endpoints", near(d_global(0.0),0.0,1e-12) && near(d_global(1.0),1.0,1e-12));
    check("dl_endpoints", near(d_local(0.0),1.0,1e-12)  && near(d_local(1.0),0.0,1e-12));
    check("dg_monotone",  d_global(0.2) < d_global(0.4));
    check("dl_monotone",  d_local(0.2)  > d_local(0.4));
    check("unit_range",   d_global(0.6)<=1.0 && d_local(0.6)<=1.0 &&
                          d_global(0.6)>=0.0 && d_local(0.6)>=0.0);

    /* raw local measure is the halo-radius * contrast-boost product */
    check("local_raw_product",
          near(halo_radius(0.3) * contrast_boost(0.3),
               R_MAX * C_MAX * (1.0-0.3) * (1.0-0.3), 1e-9));

    /* cost curves */
    check("cost_global_under",    cost_ms(ALG_GLOBAL) <= BUDGET_MS);
    check("cost_local_over",      cost_ms(ALG_LOCAL)  >  BUDGET_MS);
    check("cost_local_expensive", cost_ms(ALG_LOCAL)  >  cost_ms(ALG_GLOBAL));

    /* three histogram peak heights */
    Tier tiers[3];
    eval_tier(&tiers[0], "LOW",     500.0);
    eval_tier(&tiers[1], "MEDIUM", 2000.0);
    eval_tier(&tiers[2], "HIGH",   8000.0);

    printf("\nmodel: d_g=pi, d_l=(1-pi)^2, pi*=%.6f, peak*=pi*M=%.1f\n",
           pstar, peakstar);
    printf("cost:  global=%.3f ms  local=%.3f ms  budget=%.1f ms  U_max=%.2f\n",
           cost_ms(ALG_GLOBAL), cost_ms(ALG_LOCAL), BUDGET_MS, U_MAX);
    printf("pi* = 1/phi^2 = (3-sqrt(5))/2\n\n");

    printf("%-7s %7s %7s %7s %7s %8s %7s %7s %8s %6s\n",
           "tier","peak","pi","d_g","d_l","best","chosen","u","cost_ms","pass");
    for (int i = 0; i < 3; i++) {
        const Tier* t = &tiers[i];
        printf("%-7s %7.0f %7.4f %7.4f %7.4f %8s %7s %7.4f %8.3f %6d\n",
               t->name, t->peak, t->pi, t->dg, t->dl,
               alg_name(t->best), alg_name(t->chosen), t->u,
               cost_ms(t->chosen), t->pass);
    }
    printf("\n");

    /* LOW & MEDIUM: below the crossover, global is best and cheap -> pass. */
    check("low_below_threshold", tiers[0].peak < peakstar);
    check("low_global_best",  tiers[0].best == ALG_GLOBAL);
    check("low_pass",         tiers[0].pass && tiers[0].chosen == ALG_GLOBAL);

    check("medium_below_threshold", tiers[1].peak < peakstar);
    check("medium_global_best", tiers[1].best == ALG_GLOBAL);
    check("medium_pass",        tiers[1].pass && tiers[1].chosen == ALG_GLOBAL);

    /* HIGH: past the crossover the distortion-optimal pick is local, but local
     * blows the compute budget; the forced global path then misses U_MAX. */
    check("high_above_threshold",   tiers[2].peak > peakstar);
    check("high_local_best",        tiers[2].best == ALG_LOCAL);
    check("high_local_within_target", tiers[2].dl <= U_MAX);
    check("high_global_misses_target", tiers[2].dg > U_MAX);
    check("high_local_over_budget", cost_ms(ALG_LOCAL) > BUDGET_MS);
    check("high_forced_global",     tiers[2].chosen == ALG_GLOBAL);
    check("high_fails",             !tiers[2].pass);

    /* required report for every tier that exceeds */
    for (int i = 0; i < 3; i++) {
        const Tier* t = &tiers[i];
        if (t->pass) continue;
        printf("EXCEED tier=%s peak=%.0f d_g=%.4f d_l=%.4f "
               "threshold_peak=%.1f threshold_pi=%.4f\n",
               t->name, t->peak, t->dg, t->dl, peakstar, pstar);
        printf("        best=%s (u=%.4f) cost=%.3fms budget=%.1fms; "
               "forced=%s (u=%.4f) cost=%.3fms; reason=%s\n",
               alg_name(t->best), (t->best==ALG_GLOBAL?t->dg:t->dl),
               cost_ms(t->best), BUDGET_MS,
               alg_name(t->chosen), t->u, cost_ms(t->chosen),
               (t->best==ALG_LOCAL && cost_ms(ALG_LOCAL)>BUDGET_MS)
                   ? "local_over_budget_forced_global_misses_target"
                   : "over_budget_or_target");
    }

    printf("\nall_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
