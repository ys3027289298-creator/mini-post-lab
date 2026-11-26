#!/usr/bin/env python3
"""Resolution-calibration experiment for screen-space bloom coverage.

The same analytic HDR lamp is sampled at three 16:9 resolutions.  The bloom
pipeline mirrors bloom/src/bloom.c: hard bright-pass followed by a normalized,
separable, clamped-edge box blur.  The radius is deliberately kept at a fixed
number of pixels, which is the screen-space coupling that makes coverage drift.

Outputs JSON + CSV so the numerical claims in the report are reproducible.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import time
from pathlib import Path

import numpy as np

RESOLUTIONS = [
    {"name": "nHD", "width": 640, "height": 360},
    {"name": "HD", "width": 1280, "height": 720},
    {"name": "FHD", "width": 1920, "height": 1080},
]

PARAMS = {
    "aspect": 16.0 / 9.0,
    "peak_luminance": 8.0,
    "luminance_sigma_height": 0.010,
    "threshold0": 1.0,
    "box_radius_px": 3,
    "visibility_cutoff": 0.05,
    "coverage_target": 1.000e-3,
    "tolerance": 0.10,
    "derivative_epsilon": 0.125,
    "bisection_iterations": 34,
}


def make_lamp(width: int, height: int, p: dict) -> np.ndarray:
    """Pixel-centre sample of one resolution-independent radial HDR lamp."""
    # Coordinates are measured in frame-height units.  Thus y spans one unit and
    # x spans width/height units; the lamp sigma is independent of resolution.
    y = (np.arange(height, dtype=np.float64) + 0.5) / height - 0.5
    x = (np.arange(width, dtype=np.float64) + 0.5) / height - width / height / 2.0
    xx, yy = np.meshgrid(x, y)
    sigma = p["luminance_sigma_height"]
    r2 = xx * xx + yy * yy
    return p["peak_luminance"] * np.exp(-r2 / (2.0 * sigma * sigma))


def box_blur(src: np.ndarray, radius: int) -> np.ndarray:
    """Normalized separable box blur with clamped edges."""
    if radius < 0:
        raise ValueError("radius must be non-negative")
    if radius == 0:
        return src.copy()

    k = 2 * radius + 1

    padded = np.pad(src, ((0, 0), (radius, radius)), mode="edge")
    cs = np.zeros((padded.shape[0], padded.shape[1] + 1), dtype=np.float64)
    cs[:, 1:] = np.cumsum(padded, axis=1, dtype=np.float64)
    horizontal = (cs[:, k : k + src.shape[1]] - cs[:, : src.shape[1]]) / k

    padded = np.pad(horizontal, ((radius, radius), (0, 0)), mode="edge")
    cs = np.zeros((padded.shape[0] + 1, padded.shape[1]), dtype=np.float64)
    cs[1:, :] = np.cumsum(padded, axis=0, dtype=np.float64)
    return (cs[k : k + src.shape[0], :] - cs[: src.shape[0], :]) / k


def coverage(lamp: np.ndarray, threshold: float, p: dict) -> tuple[float, int, np.ndarray]:
    bright = np.maximum(lamp - threshold, 0.0)
    glow = box_blur(bright, p["box_radius_px"])
    count = int(np.count_nonzero(glow >= p["visibility_cutoff"]))
    total = lamp.shape[0] * lamp.shape[1]
    return count / total, count, glow


def slope_at_threshold(lamp: np.ndarray, threshold: float, p: dict) -> float:
    eps = p["derivative_epsilon"]
    a_plus, _, _ = coverage(lamp, threshold + eps, p)
    a_minus, _, _ = coverage(lamp, threshold - eps, p)
    return (a_plus - a_minus) / (2.0 * eps)


def solve_threshold(lamp: np.ndarray, target: float, p: dict) -> tuple[float, float, int]:
    """Bisection on the monotone non-increasing coverage step function."""
    lo, hi = 0.0, p["peak_luminance"] - 1e-12
    if coverage(lamp, lo, p)[0] < target:
        raise RuntimeError("target coverage is above the threshold=0 attainable coverage")
    if coverage(lamp, hi, p)[0] > target:
        raise RuntimeError("target coverage is below the maximum-threshold attainable coverage")

    for _ in range(p["bisection_iterations"]):
        mid = 0.5 * (lo + hi)
        if coverage(lamp, mid, p)[0] > target:
            lo = mid  # threshold is too low: raise it
        else:
            hi = mid
    threshold = 0.5 * (lo + hi)
    area, count, _ = coverage(lamp, threshold, p)
    return threshold, area, count


def pair_differences(areas: list[float]) -> list[dict]:
    pairs = []
    for i in range(len(areas)):
        for j in range(i + 1, len(areas)):
            low, high = sorted((areas[i], areas[j]))
            symmetric = abs(areas[i] - areas[j]) / (0.5 * (areas[i] + areas[j]))
            pairs.append(
                {
                    "i": i,
                    "j": j,
                    "relative_to_smaller": high / low - 1.0,
                    "symmetric_mean_denominator": symmetric,
                }
            )
    return pairs


def critical_geometric_resolution(p: dict, a0: float, kappa: float) -> dict:
    """Critical heights from A(h)/A(h0) = ((a0 + kappa*r/h)/(a0 + kappa*r/h0))^2."""
    h0 = RESOLUTIONS[1]["height"]
    r = p["box_radius_px"]
    b0 = kappa * r / h0
    eps = p["tolerance"]

    low_den = math.sqrt(1.0 + eps) * (a0 + b0) - a0
    high_den = math.sqrt(1.0 - eps) * (a0 + b0) - a0
    h_low_fail = kappa * r / low_den if low_den > 0 else None
    h_high_fail = kappa * r / high_den if high_den > 0 else None
    max_high_saving = 1.0 - (a0 / (a0 + b0)) ** 2
    return {
        "reference_height": h0,
        "kappa_calibrated": kappa,
        "source_radius_height_units": a0,
        "reference_fringe_height_units": b0,
        "fail_for_h_lte": h_low_fail,
        "fail_for_h_gte": h_high_fail,
        "high_side_max_relative_loss": max_high_saving,
        "high_side_10pct_possible": max_high_saving >= eps,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, default=None)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    out_dir = args.out_dir or (root / "artifacts")
    out_dir.mkdir(parents=True, exist_ok=True)

    p = dict(PARAMS)
    rows = []
    lamps = []

    for spec in RESOLUTIONS:
        w, h = spec["width"], spec["height"]
        lamp = make_lamp(w, h, p)
        lamps.append(lamp)

        t0 = time.perf_counter()
        area0, count0, _ = coverage(lamp, p["threshold0"], p)
        baseline_ms = (time.perf_counter() - t0) * 1000.0

        derivative = slope_at_threshold(lamp, p["threshold0"], p)
        newton_delta = (p["coverage_target"] - area0) / derivative
        threshold_c, area_c, count_c = solve_threshold(lamp, p["coverage_target"], p)

        rows.append(
            {
                **spec,
                "pixels": w * h,
                "baseline_threshold": p["threshold0"],
                "baseline_area_fraction": area0,
                "baseline_area_pixels": count0,
                "baseline_run_ms": baseline_ms,
                "area_derivative_per_luminance": derivative,
                "newton_correction_to_target": newton_delta,
                "corrected_threshold": threshold_c,
                "correction_delta": threshold_c - p["threshold0"],
                "corrected_area_fraction": area_c,
                "corrected_area_pixels": count_c,
            }
        )

    baseline_areas = [r["baseline_area_fraction"] for r in rows]
    corrected_areas = [r["corrected_area_fraction"] for r in rows]
    baseline_pairs = pair_differences(baseline_areas)
    corrected_pairs = pair_differences(corrected_areas)
    baseline_spread = max(baseline_areas) / min(baseline_areas) - 1.0
    corrected_spread = max(corrected_areas) / min(corrected_areas) - 1.0

    worst_pair = max(baseline_pairs, key=lambda x: x["relative_to_smaller"])
    i, j = worst_pair["i"], worst_pair["j"]
    # Newton correction that would move the higher-resolution member onto the
    # lower-resolution member's measured area.
    if baseline_areas[i] > baseline_areas[j]:
        low_i, high_i = i, j
    else:
        low_i, high_i = j, i
    worst_newton_match = (
        (baseline_areas[low_i] - baseline_areas[high_i])
        / rows[high_i]["area_derivative_per_luminance"]
    )
    worst_exact_threshold, worst_exact_area, worst_exact_count = solve_threshold(
        lamps[high_i], baseline_areas[low_i], p
    )

    # Rough geometric fringe coefficient from the middle tier's measured radius.
    q = p["aspect"]
    a0 = p["luminance_sigma_height"] * math.sqrt(
        2.0 * math.log(p["peak_luminance"] / p["threshold0"])
    )
    amid = baseline_areas[1]
    r_mid = math.sqrt(q * amid / math.pi)
    kappa = (r_mid - a0) * RESOLUTIONS[1]["height"] / p["box_radius_px"]
    critical = critical_geometric_resolution(p, a0, kappa)

    # Cost model: one coverage evaluation is one full-frame bright pass + blur.
    # Express cost in 720p-equivalent full-frame coverage evaluations.
    n720 = RESOLUTIONS[1]["width"] * RESOLUTIONS[1]["height"]
    eval_cost = {
        r["name"]: r["pixels"] / n720 for r in rows
    }
    bisection_cost_per_switch = p["bisection_iterations"] * max(eval_cost.values())
    newton_cost_per_switch = 3.0 * max(eval_cost.values())  # base/probe+/probe-
    three_tier_calibration_cost = p["bisection_iterations"] * sum(eval_cost.values())

    result = {
        "params": p,
        "rows": rows,
        "baseline_pair_differences": baseline_pairs,
        "corrected_pair_differences": corrected_pairs,
        "baseline_max_relative_spread_vs_smaller": baseline_spread,
        "corrected_max_relative_spread_vs_smaller": corrected_spread,
        "requirement": {
            "max_allowed_spread": p["tolerance"],
            "baseline_passes": baseline_spread < p["tolerance"],
            "corrected_passes": corrected_spread < p["tolerance"],
        },
        "worst_pair": {
            "low_area_index": low_i,
            "high_area_index": high_i,
            "low_resolution": f'{rows[low_i]["width"]}x{rows[low_i]["height"]}',
            "high_resolution": f'{rows[high_i]["width"]}x{rows[high_i]["height"]}',
            "low_area_fraction": baseline_areas[low_i],
            "high_area_fraction": baseline_areas[high_i],
            "low_area_pixels": rows[low_i]["baseline_area_pixels"],
            "high_area_pixels": rows[high_i]["baseline_area_pixels"],
            "newton_delta_for_high_to_match_low": worst_newton_match,
            "exact_high_threshold_to_match_low": worst_exact_threshold,
            "exact_high_delta_to_match_low": worst_exact_threshold - p["threshold0"],
            "exact_matched_area_fraction": worst_exact_area,
            "exact_matched_area_pixels": worst_exact_count,
        },
        "geometric_critical_resolution": critical,
        "cost_model_720p_equivalent_evaluations": {
            "per_resolution": eval_cost,
            "bisection_per_switch_worst_tier": bisection_cost_per_switch,
            "newton_per_switch_worst_tier": newton_cost_per_switch,
            "one_time_three_tier_bisection": three_tier_calibration_cost,
        },
    }

    json_path = out_dir / "bloom_resolution_results.json"
    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    csv_path = out_dir / "bloom_resolution_results.csv"
    fields = [
        "name", "width", "height", "pixels",
        "baseline_area_pixels", "baseline_area_fraction",
        "area_derivative_per_luminance", "newton_correction_to_target",
        "corrected_threshold", "correction_delta",
        "corrected_area_pixels", "corrected_area_fraction",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: row[k] for k in fields})

    print(json.dumps({
        "json": str(json_path),
        "csv": str(csv_path),
        "baseline_spread": baseline_spread,
        "corrected_spread": corrected_spread,
        "baseline_passes": result["requirement"]["baseline_passes"],
        "corrected_passes": result["requirement"]["corrected_passes"],
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
