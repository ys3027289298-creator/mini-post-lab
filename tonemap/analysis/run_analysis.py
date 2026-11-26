"""End-to-end acceptance run: three scene brightness tiers x three curves.

For every tier and every curve family it reports
  * the feasible parameter interval from the D/C constraint intersection,
  * the operating point (D = D* boundary: max contrast that still passes the
    dark-detail rule),
  * analytic and empirical (sampled-image histogram) values of D and C,
  * the shadow-gain budget check that carries the DR infeasibility verdict.

Usage:  python3 run_analysis.py [--samples N] [--bins B]
Outputs: printed table plus outputs/acceptance_run.json
"""

import argparse
import json
import os

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
    evaluate_scene,
    range_threshold,
    shadow_gain,
)

# ---------------------------------------------------------------------------
# Executable acceptance thresholds (the numbers turn the prose sentence into
# a test).  D*: at least 75% of the dark-zone bins must stay populated by the
# shadow cohort.  C*: the 90/10 percentile ratio must stay at least 4 (a
# deliberately modest floor; all three families clear it near the linear
# endpoint).  G: no shadow bin may receive more than 40x linear-light gain
# (noise/banding budget of the display path).
# ---------------------------------------------------------------------------
D_STAR = 0.75
C_STAR = 4.0
G_BUDGET = 40.0

SCENES = [
    ("bright", 64.0),    # 6 stops
    ("medium", 256.0),   # 8 stops
    ("dark", 4096.0),    # 12 stops
]

CURVE_BUILDERS = {
    "power": (lambda p: (lambda u: power_curve(u, p))),
    "log": (lambda p: (lambda u: log_curve(u, p))),
    "piecewise_linear": (lambda p: (lambda u: piecewise_linear_curve(u, p))),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=1_000_000)
    ap.add_argument("--bins", type=int, default=500)
    ap.add_argument("--seed", type=int, default=20261127)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    R_star, R_asym = range_threshold(D_STAR, G_BUDGET)
    report = {
        "thresholds": {"D_star": D_STAR, "C_star": C_STAR, "G": G_BUDGET,
                       "dark_zone_fraction": DARK_EDGE},
        "range_limit": {"R_star": R_star, "stops_star": float(np.log2(R_star)),
                        "asymptotic_R": R_asym,
                        "form": "R*: root of R = (5G/D*)(R^0.2 - 1)"},
        "log_family_limits": {"C_min": 9.0, "C_max_supremum": float(LOG_C_MAX)},
        "scenes": [],
    }

    print(f"Acceptance: D >= {D_STAR}, C >= {C_STAR}, shadow gain <= {G_BUDGET}x")
    print(f"DR threshold R* = {R_star:.2f} ({np.log2(R_star):.2f} stops), "
          f"asymptotic estimate {R_asym:.1f}\n")

    for name, R in SCENES:
        lum = sample_scene(R, args.samples, rng)
        scene_entry = {"name": name, "R": R, "stops": float(np.log2(R)),
                       "over_dr_limit": bool(R > R_star), "families": []}
        print(f"=== scene {name:6s} R={R:5.0f} ({np.log2(R):4.1f} stops) "
              f"{'[DR OVER LIMIT]' if R > R_star else ''}")
        verdicts = evaluate_scene(name, R, D_STAR, C_STAR, G_BUDGET)
        for v in verdicts:
            entry = {"family": v.family, "feasible_shape": v.interval is not None,
                     "interval": list(v.interval) if v.interval else None,
                     "param_pick": None if np.isnan(v.param_pick) else float(v.param_pick),
                     "D_analytic": float(v.D_pred), "C_analytic": float(v.C_pred),
                     "gain": float(v.gain), "passes": bool(v.feasible),
                     "reason": v.reason}
            if v.interval is not None:
                builder = CURVE_BUILDERS[v.family](v.param_pick)
                m = empirical_metrics(lum, R, builder, n_bins=args.bins)
                literal = literal_fullhist_D(lum, R, builder, n_bins=args.bins)
                entry.update(D_empirical=m.D, C_empirical=m.C,
                             shadow_pixels=m.n_shadow, filled_bins=m.n_filled,
                             dark_bins=m.n_dark_bins,
                             literal_wholehist_D=literal,
                             D_error=abs(m.D - v.D_pred),
                             C_error=abs(m.C - v.C_pred))
                print(f"  {v.family:18s} p={v.param_pick:9.5f} "
                      f"D={v.D_pred:.3f}(emp {m.D:.3f}) "
                      f"C={v.C_pred:7.3f}(emp {m.C:7.3f}) "
                      f"gain={v.gain:8.2f} "
                      f"{'PASS' if v.feasible else 'FAIL: ' + v.reason}")
            else:
                print(f"  {v.family:18s} no feasible parameter interval ({v.reason})")
            scene_entry["families"].append(entry)

        # Independent infeasibility statement for the over-limit tier.
        if R > R_star:
            min_gain = shadow_gain(R, D_STAR)
            scene_entry["infeasibility"] = (
                f"R={R:.0f} > R*={R_star:.1f}: even the best possible curve needs "
                f"gain {min_gain:.1f}x > budget {G_BUDGET}x to reach D={D_STAR}; "
                f"no curve of any family can satisfy both metrics")
            print(f"  -> {scene_entry['infeasibility']}\n")
        report["scenes"].append(scene_entry)

    out_dir = os.path.join(os.path.dirname(__file__), "outputs")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "acceptance_run.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
