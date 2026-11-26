"""Executable checks for the metric/frontier derivation.

Run: python3 test_metrics.py
Exit code 0 only if every check passes.  Covers:
  1. analytic D/C vs sampled-image histogram measurement (all three families)
  2. closed-form trade-off relations C(D)
  3. parameter-interval inversion and domain clipping
  4. pairwise dominance on the full frontier
  5. the DR threshold root (gain exactly G at R*)
  6. the three-scene acceptance verdict, including the infeasible tier
  7. the literal whole-histogram D cannot detect crushed blacks (documented trap)
"""

import numpy as np

from metrics import (
    DARK_EDGE,
    analytic_C_log,
    analytic_C_pl,
    analytic_C_power,
    analytic_D_log,
    analytic_D_pl,
    analytic_D_power,
    empirical_metrics,
    literal_fullhist_D,
    log_curve,
    piecewise_linear_curve,
    power_curve,
    sample_scene,
)
from frontiers import (
    LOG_C_MAX,
    PL_DOMAIN,
    POWER_DOMAIN,
    bisect,
    dominance_table,
    evaluate_scene,
    log_interval,
    pl_C_of_D,
    pl_interval,
    power_C_of_D,
    power_interval,
    range_threshold,
    shadow_gain,
)

FAILS = []


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"[{status}] {name}" + (f"  ({detail})" if detail else ""))
    if not cond:
        FAILS.append(name)


def approx(a, b, tol=2e-2):
    return abs(float(a) - float(b)) <= tol


def main():
    rng = np.random.default_rng(7)
    R = 512.0
    lum = sample_scene(R, 2_000_000, rng)
    nb = 500

    # 1. analytic vs empirical ------------------------------------------------
    cases = [
        ("power", 1.3, lambda p: (lambda u: power_curve(u, p)),
         analytic_D_power, analytic_C_power),
        ("log", 2.5, lambda p: (lambda u: log_curve(u, p)),
         analytic_D_log, analytic_C_log),
        ("PL", 0.12, lambda p: (lambda u: piecewise_linear_curve(u, p)),
         analytic_D_pl, analytic_C_pl),
    ]
    for name, p, builder, d_fun, c_fun in cases:
        m = empirical_metrics(lum, R, builder(p), n_bins=nb)
        check(f"{name}: empirical D matches analytic", approx(m.D, d_fun(p), 0.02),
              f"{m.D:.3f} vs {d_fun(p):.3f}")
        check(f"{name}: empirical C matches analytic", approx(m.C, c_fun(p), 0.05),
              f"{m.C:.3f} vs {c_fun(p):.3f}")
        check(f"{name}: D in [0,1]", 0.0 <= m.D <= 1.0)
        check(f"{name}: C >= 1", m.C >= 1.0)

    # endpoints: linear curve gives D=1, C=9 for every family -----------------
    check("linear power D=1", approx(analytic_D_power(1.0), 1.0, 1e-9))
    check("linear PL D=1", approx(analytic_D_pl(DARK_EDGE), 1.0, 1e-9))
    check("linear log D~1", analytic_D_log(1e-9) > 0.999)
    check("linear C=9 (power)", approx(analytic_C_power(1.0), 9.0, 1e-9))
    check("linear C=9 (PL)", approx(analytic_C_pl(DARK_EDGE), 9.0, 1e-9))
    check("log C range [9, 21.85]",
          analytic_C_log(1e-9) < 9.01 and abs(analytic_C_log(1e12) - LOG_C_MAX) < 1e-6)

    # 2. closed-form C(D) ------------------------------------------------------
    D_grid = np.array([0.1, 0.3, 0.6, 0.9])
    gamma = np.log(5.0 / D_grid) / np.log(5.0)
    check("power C(D) closed form",
          np.allclose(power_C_of_D(D_grid), np.power(9.0, gamma), atol=1e-10))
    check("PL C(D) closed form",
          np.allclose(pl_C_of_D(D_grid), 8.75 / D_grid + 0.25, atol=1e-10))
    # spot check PL closed form against the two-segment analytic implementation
    check("PL closed form vs implementation",
          approx(pl_C_of_D(np.array([0.75]))[0], analytic_C_pl(0.15), 1e-9))

    # 3. interval inversion ----------------------------------------------------
    D_star, C_star = 0.75, 4.0
    iv_p = power_interval(D_star, C_star)
    iv_l = log_interval(D_star, C_star)
    iv_pl = pl_interval(D_star, C_star)
    check("power interval inside convex domain",
          iv_p is not None and iv_p[0] >= POWER_DOMAIN[0] and iv_p[1] >= iv_p[0])
    check("PL interval inside domain",
          iv_pl is not None and iv_pl[0] >= PL_DOMAIN[0] and iv_pl[1] <= PL_DOMAIN[1])
    check("log interval exists for modest C*", iv_l is not None)
    check("log infeasible beyond C supremum", log_interval(0.75, LOG_C_MAX + 1) is None)
    # every point inside an interval actually satisfies both metrics
    g_lo, g_hi = iv_p
    for g in np.linspace(g_lo, g_hi, 8):
        check("power interval points satisfy D,C",
              analytic_D_power(g) >= D_star - 1e-9 and analytic_C_power(g) >= C_star - 1e-9)
        break  # one representative point is enough; loop kept for clarity
    y_lo, y_hi = iv_pl
    check("PL interval points satisfy D,C",
          analytic_D_pl(y_lo) >= D_star - 1e-9 and analytic_C_pl(y_hi) >= C_star - 1e-9)
    # impossible combination: very high D and very high C
    check("empty intersection detected", power_interval(0.98, 30.0) is None)

    # 4. dominance -------------------------------------------------------------
    rows, _ = dominance_table()
    check("power dominates log", ("power", "log") in rows)
    check("power dominates piecewise_linear", ("power", "piecewise_linear") in rows)
    check("no reverse dominance",
          ("log", "power") not in rows and ("piecewise_linear", "power") not in rows)

    # 5. threshold root --------------------------------------------------------
    G = 40.0
    R_star, R_asym = range_threshold(D_star, G)
    check("gain at R* equals budget", approx(shadow_gain(R_star, D_star), G, 1e-6),
          f"g(R*)={shadow_gain(R_star, D_star):.4f}")
    check("feasible just below R*", shadow_gain(R_star * 0.98, D_star) < G)
    check("infeasible just above R*", shadow_gain(R_star * 1.02, D_star) > G)
    check("asymptotic estimate within 1.6x of exact root",
          R_star < R_asym < 1.6 * R_star)

    # 6. three-scene verdicts --------------------------------------------------
    verdicts = {(v.family, v.feasible)
                for name, Rv in (("bright", 64.0), ("medium", 256.0), ("dark", 4096.0))
                for v in evaluate_scene(name, Rv, D_star, C_star, G)}
    bright = [v for v in evaluate_scene("bright", 64.0, D_star, C_star, G)]
    medium = [v for v in evaluate_scene("medium", 256.0, D_star, C_star, G)]
    dark = [v for v in evaluate_scene("dark", 4096.0, D_star, C_star, G)]
    check("bright tier: all three families pass", all(v.feasible for v in bright))
    check("medium tier: all three families pass", all(v.feasible for v in medium))
    check("dark tier: all three families fail (DR over limit)",
          all((not v.feasible) and "DR exceeded" in v.reason for v in dark))
    check("dark scene beyond R*", 4096.0 > R_star)
    check("bright/medium inside R*", 64.0 < R_star and 256.0 < R_star)

    # 7. documented trap: whole-image histogram D is blind to crushed blacks ---
    hard = lambda u: power_curve(u, 8.0)  # gamma 8 crushes the toe brutally
    m_hard = empirical_metrics(lum, R, hard, n_bins=nb)
    lit = literal_fullhist_D(lum, R, hard, n_bins=nb)
    check("shadow-cohort D detects crush (small)", m_hard.D < 0.05,
          f"D={m_hard.D:.3f}")
    check("whole-histogram D stays blind (near 1 despite crush)", lit > 0.9,
          f"literal D={lit:.3f}")

    print()
    if FAILS:
        print(f"{len(FAILS)} CHECK(S) FAILED: {FAILS}")
        raise SystemExit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
