#!/usr/bin/env -S python3 -I
"""Analyze drift_lab CSV output and emit report/data.js for the HTML charts."""
import csv
import json
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out")
REPORT = os.path.join(ROOT, "report")

FPS = 60
Q = 1.0 / 256.0
BUDGET = 0.5 * Q
N10 = int(FPS * 600)


def read_csv(name):
    with open(os.path.join(OUT, name), newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def fnum(row, key):
    return float(row[key])


def fit_exponent(rows):
    """Least-squares slope in log-log coordinates: D ~ N**alpha."""
    xs = [math.log(fnum(r, "frames")) for r in rows]
    ys = [math.log(fnum(r, "rms_norm")) for r in rows]
    xm = sum(xs) / len(xs)
    ym = sum(ys) / len(ys)
    num = sum((x - xm) * (y - ym) for x, y in zip(xs, ys))
    den = sum((x - xm) ** 2 for x in xs)
    alpha = num / den
    # Constrained-shape residuals give a simple shape decision.
    def sse(fixed_alpha):
        # Intercept is fitted under the fixed exponent.
        cs = [y - fixed_alpha * x for x, y in zip(xs, ys)]
        c = sum(cs) / len(cs)
        return sum((y - (fixed_alpha * x + c)) ** 2 for x, y in zip(xs, ys))
    return {
        "alpha": alpha,
        "sse_linear": sse(1.0),
        "sse_sqrt": sse(0.5),
        "decision": "linear" if sse(1.0) < sse(0.5) else "sqrt",
    }


runs_raw = read_csv("runs.csv")
reset_curves = read_csv("reset_curves.csv")
continuity = read_csv("continuity_check.csv")

runs = []
by_model = {"linear": [], "sqrt": []}
for r in runs_raw:
    rec = {
        "model": r["model"],
        "tier": r["tier"],
        "duration_s": int(r["duration_s"]),
        "duration_min": int(r["duration_s"]) / 60,
        "frames": int(r["frames"]),
        "lanes": int(r["lanes"]),
        "signed_mean_norm": fnum(r, "signed_mean_norm"),
        "rms_norm": fnum(r, "rms_norm"),
        "max_abs_norm": fnum(r, "max_abs_norm"),
        "offset_quant": fnum(r, "rms_norm") / Q,
        "signed_mean_quant": fnum(r, "signed_mean_norm") / Q,
        "max_abs_quant": fnum(r, "max_abs_norm") / Q,
        "policy_r_frames": int(r["policy_r_frames"]),
        "boundary_rms_norm": fnum(r, "boundary_rms_norm"),
        "boundary_rms_quant": fnum(r, "boundary_rms_norm") / Q,
        "policy_max_abs_norm": fnum(r, "policy_max_abs_norm"),
        "policy_max_abs_quant": fnum(r, "policy_max_abs_norm") / Q,
        "bare_pass": fnum(r, "rms_norm") <= BUDGET,
        "policy_pass": int(r["rms_pass"]) == 1 and int(r["max_pass"]) == 1,
    }
    runs.append(rec)
    by_model[r["model"]].append(rec)

fits = {m: fit_exponent(rows) for m, rows in by_model.items()}

# Inversions from the stated field measurement D(600 s) = half quantum.
estimates = {}
for label, quantum in (("q_1_256", 1 / 256), ("q_1_255", 1 / 255)):
    budget = quantum / 2
    estimates[label] = {
        "quantum": quantum,
        "budget": budget,
        "n10": N10,
        "delta_per_frame": budget / N10,
        "sigma_per_frame_rms": budget / math.sqrt(N10),
        "delta_per_second": FPS * budget / N10,
        "sigma_per_sqrt_second": budget / math.sqrt(600),
        "reset_linear_s": (budget / (budget / N10)) / FPS,
        "reset_sqrt_s": ((budget / (budget / math.sqrt(N10))) ** 2) / FPS,
        "reset_linear_80pct_s": 0.8 * N10 / FPS,
        "reset_sqrt_80pct_s": (0.8 ** 2 * N10) / FPS,
    }

# Ideal duration curves, normalized to the same 10-minute anchor.
runtime_curves = []
for minute in [1, 2, 3, 5, 7, 10, 15, 20, 30, 45, 60]:
    ratio = minute / 10.0
    runtime_curves.append({
        "minutes": minute,
        "linear_quant": 0.5 * ratio,
        "sqrt_quant": 0.5 * math.sqrt(ratio),
    })

reset = [{
    "r": int(r["reset_frames"]),
    "seconds": fnum(r, "reset_seconds"),
    "linear_quant": fnum(r, "linear_drift_quant"),
    "sqrt_quant": fnum(r, "sqrt_drift_rms_quant"),
    "continuity": fnum(r, "continuity"),
} for r in reset_curves]

cont = [{
    "r": int(r["reset_frames"]),
    "seconds": fnum(r, "reset_seconds"),
    "diff_rms": fnum(r, "measured_diff_rms"),
    "theory_diff_rms": fnum(r, "theory_diff_rms"),
    "continuity": fnum(r, "measured_continuity"),
} for r in continuity]

data = {
    "fps": FPS,
    "q": Q,
    "budget": BUDGET,
    "budget_quant": 0.5,
    "n10": N10,
    "policy_r": 600,
    "policy_seconds": 10.0,
    "estimates": estimates,
    "fits": fits,
    "runs": runs,
    "runtime_curves": runtime_curves,
    "reset_curves": reset,
    "continuity_check": cont,
}

os.makedirs(REPORT, exist_ok=True)
with open(os.path.join(REPORT, "data.js"), "w", encoding="utf-8") as f:
    f.write("window.DRIFT_DATA = ")
    json.dump(data, f, ensure_ascii=False, indent=2)
    f.write(";\n")

with open(os.path.join(OUT, "analysis.json"), "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("exponent fits:")
for model, fit in fits.items():
    print(f"  {model}: alpha={fit['alpha']:.4f} decision={fit['decision']} "
          f"sse_lin={fit['sse_linear']:.5g} sse_sqrt={fit['sse_sqrt']:.5g}")
print("runs:")
for r in runs:
    print(f"  {r['model']:6s} {r['duration_min']:>4.0f}min "
          f"D={r['offset_quant']:.4f}q bare_pass={r['bare_pass']} "
          f"policy_boundary={r['boundary_rms_quant']:.5f}q "
          f"policy_max={r['policy_max_abs_quant']:.4f}q")
print(f"wrote {os.path.join(REPORT, 'data.js')}")
