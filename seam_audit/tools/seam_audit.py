#!/usr/bin/env python3
"""Audit half-open chunk ownership and exposure-induced seam jumps.

The fixture is deliberately deterministic and dependency-free.  It is not a
reconstruction of an unavailable export; it is a reproducible regression target
with the same dimensions as a small square animation frame.
"""

import csv
import json
import math
import os

W = 256
H = 256
FRAMES = 12
CHUNK_COUNTS = (2, 4, 8)
BITS = 8
CODE_SPAN = (1 << BITS) - 1  # 255 intervals between code 0 and code 255
KEY = 0.18
BLUR_RADIUS = 4

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULT_DIR = os.path.join(ROOT, "results")


def luminance(frame, x, y):
    """Deterministic linear-light fixture in [0, 1]."""
    t = frame / (FRAMES - 1)
    v = 0.20 + 0.18 * t
    v += 0.10 * math.sin(2.0 * math.pi * x / 128.0 + 0.4 * frame)
    v += 0.06 * math.cos(2.0 * math.pi * y / 96.0 + 0.3 * frame)
    v += 0.04 * math.sin((x + y) / 17.0 + frame)

    # Sharp columns on likely partition boundaries make ownership errors visible.
    if x % 64 == 0:
        z = 0.10 + 0.80 * (0.5 + 0.5 * math.sin((frame + 1) * 1.17 + x * 0.031))
        v = 0.55 * v + 0.45 * z
    return max(0.0, min(1.0, v))


def boundaries(k):
    return [i * W // k for i in range(k + 1)]


def column_sums(frame):
    sums = [0.0] * W
    for x in range(W):
        total = 0.0
        for y in range(H):
            total += luminance(frame, x, y)
        sums[x] = total
    return sums


def exposure_gain(sum_lum, pixel_count):
    mean_lum = sum_lum / pixel_count
    return KEY / max(mean_lum, 1e-6)


def reinhard_code(value, gain):
    mapped = value * gain
    normalized = mapped / (1.0 + mapped)
    return CODE_SPAN * normalized


def ownership_audit(k, duplicate_boundary=True):
    """Return per-column multiplicity and error counts.

    Correct vertical strips use [b_i, b_{i+1}).  The buggy inclusive writer also
    adds b_{i+1} to chunk i, so every internal boundary column has multiplicity 2.
    """
    bs = boundaries(k)
    multiplicity = [0] * W
    for i in range(k):
        for x in range(bs[i], bs[i + 1]):
            multiplicity[x] += 1

    duplicate_columns = 0
    missing_columns = 0
    if duplicate_boundary:
        for b in bs[1:-1]:
            multiplicity[b] += 1

    for count in multiplicity:
        if count == 0:
            missing_columns += 1
        elif count > 1:
            duplicate_columns += count - 1

    return {
        "multiplicity_min": min(multiplicity),
        "multiplicity_max": max(multiplicity),
        "duplicate_pixels": duplicate_columns * H,
        "missing_pixels": missing_columns * H,
        "wrong_pixels": (duplicate_columns + missing_columns) * H,
    }


def run_frame(k, frame, col_sum):
    bs = boundaries(k)
    gains_strict = []
    gains_dup = []

    for i in range(k):
        x0, x1 = bs[i], bs[i + 1]
        total = sum(col_sum[x0:x1])
        count = (x1 - x0) * H
        gains_strict.append(exposure_gain(total, count))

        dup_total = total
        dup_count = count
        if i < k - 1:
            # Left chunk incorrectly includes the first column of the next chunk.
            dup_total += col_sum[x1]
            dup_count += H
        gains_dup.append(exposure_gain(dup_total, dup_count))

    worst_owner = {
        "code": 0.0,
        "boundary_index": None,
        "x": None,
        "y": None,
        "left_delta_code": 0.0,
        "right_delta_code": 0.0,
    }
    worst_absolute = 0.0

    for boundary_index in range(1, k):
        x = bs[boundary_index]
        left = boundary_index - 1
        right = boundary_index
        for y in range(H):
            value = luminance(frame, x, y)
            strict_left = reinhard_code(value, gains_strict[left])
            strict_right = reinhard_code(value, gains_strict[right])
            dup_left = reinhard_code(value, gains_dup[left])
            dup_right = reinhard_code(value, gains_dup[right])

            delta_left = dup_left - strict_left
            delta_right = dup_right - strict_right
            # Difference of cross-boundary steps: natural image contrast cancels,
            # leaving the discontinuity introduced by ownership/exposure error.
            owner_jump = abs(delta_right - delta_left)
            absolute_jump = abs(dup_right - dup_left)
            if absolute_jump > worst_absolute:
                worst_absolute = absolute_jump
            if owner_jump > worst_owner["code"]:
                worst_owner = {
                    "code": owner_jump,
                    "boundary_index": boundary_index,
                    "x": x,
                    "y": y,
                    "left_delta_code": delta_left,
                    "right_delta_code": delta_right,
                }

    # A correctly merged global histogram gives one gain for the whole frame,
    # so the same source value at a chunk boundary maps to exactly one code.
    global_seam_code = 0.0

    return {
        "frame": frame + 1,
        "max_owner_artifact_code": worst_owner["code"],
        "max_absolute_buggy_local_seam_code": worst_absolute,
        "max_global_exposure_seam_code": global_seam_code,
        "worst_boundary_index": worst_owner["boundary_index"],
        "worst_x": worst_owner["x"],
        "worst_y": worst_owner["y"],
        "left_delta_code": worst_owner["left_delta_code"],
        "right_delta_code": worst_owner["right_delta_code"],
        "pass_one_quantum": worst_owner["code"] <= 1.0,
    }


def horizontal_box_blur(frame_values, x, y, radius, available_lo, available_hi):
    total = 0.0
    for offset in range(-radius, radius + 1):
        xx = x + offset
        if xx < available_lo:
            xx = available_lo
        if xx >= available_hi:
            xx = available_hi - 1
        xx = max(0, min(W - 1, xx))
        total += frame_values[y][xx]
    return total / (2 * radius + 1)


def overlap_curve():
    """Quality/cost curve for a horizontal box blur split into K=8 strips."""
    k = 8
    bs = boundaries(k)
    frames = [
        [[luminance(f, x, y) for x in range(W)] for y in range(H)]
        for f in range(FRAMES)
    ]

    quality = []
    for r in range(BLUR_RADIUS + 2):
        worst_code = 0.0
        worst_frame = None
        for f, pixels in enumerate(frames):
            global_blur = [
                [
                    horizontal_box_blur(pixels, x, y, BLUR_RADIUS, 0, W)
                    for x in range(W)
                ]
                for y in range(H)
            ]
            for chunk in range(k):
                own_lo, own_hi = bs[chunk], bs[chunk + 1]
                available_lo = max(0, own_lo - r)
                available_hi = min(W, own_hi + r)
                for y in range(H):
                    for x in range(own_lo, own_hi):
                        local = horizontal_box_blur(
                            pixels, x, y, BLUR_RADIUS,
                            available_lo, available_hi,
                        )
                        error = CODE_SPAN * abs(local - global_blur[y][x])
                        if error > worst_code:
                            worst_code = error
                            worst_frame = f + 1
        quality.append({
            "halo_radius_px": r,
            "overlap_band_px": 2 * r,
            "worst_blur_seam_error_code": worst_code,
            "worst_frame": worst_frame,
            "quality_pass": worst_code <= 1.0,
        })

    cost = []
    for kk in CHUNK_COUNTS:
        for r in range(BLUR_RADIUS + 2):
            # Edge strips have one halo; K-1 internal boundaries add 2r columns.
            ratio = 1.0 + 2.0 * r * (kk - 1) / W
            cost.append({
                "chunks": kk,
                "halo_radius_px": r,
                "overlap_band_px": 2 * r,
                "compute_ratio": ratio,
                "compute_overhead_pct": 100.0 * (ratio - 1.0),
            })
    return quality, cost


def main():
    os.makedirs(RESULT_DIR, exist_ok=True)
    n_pixels = W * H
    error_budget = math.floor(n_pixels / CODE_SPAN)

    tiers = []
    metric_rows = []
    for k in CHUNK_COUNTS:
        ownership = ownership_audit(k, duplicate_boundary=True)
        frame_results = []
        for f in range(FRAMES):
            cols = column_sums(f)
            result = run_frame(k, f, cols)
            frame_results.append(result)
            metric_rows.append({
                "chunks": k,
                "frame": result["frame"],
                "duplicate_pixels": ownership["duplicate_pixels"],
                "owner_artifact_code": result["max_owner_artifact_code"],
                "absolute_buggy_local_seam_code":
                    result["max_absolute_buggy_local_seam_code"],
                "pass_one_quantum": result["pass_one_quantum"],
                "worst_x": result["worst_x"],
                "worst_y": result["worst_y"],
            })

        failed_frames = [
            row["frame"] for row in frame_results if not row["pass_one_quantum"]
        ]
        worst = max(frame_results, key=lambda row: row["max_owner_artifact_code"])
        bound_code = CODE_SPAN * ownership["wrong_pixels"] / n_pixels
        tiers.append({
            "chunks": k,
            "boundaries": boundaries(k),
            "ownership": ownership,
            "global_count_bias_fraction":
                ownership["duplicate_pixels"] / n_pixels,
            "worst_case_count_bound_code": bound_code,
            "count_budget_pass": bound_code <= 1.0,
            "measured_pass": not failed_frames,
            "failed_frames": failed_frames,
            "worst_frame": worst["frame"],
            "worst_owner_artifact_code":
                worst["max_owner_artifact_code"],
            "worst_location": {
                "boundary_index": worst["worst_boundary_index"],
                "x": worst["worst_x"],
                "y": worst["worst_y"],
            },
            "frames": frame_results,
        })

    strict = []
    for k in CHUNK_COUNTS:
        ownership = ownership_audit(k, duplicate_boundary=False)
        strict.append({
            "chunks": k,
            "multiplicity_min": ownership["multiplicity_min"],
            "multiplicity_max": ownership["multiplicity_max"],
            "duplicate_pixels": ownership["duplicate_pixels"],
            "missing_pixels": ownership["missing_pixels"],
            "histogram_merge": "global",
            "exposure_scope": "frame",
            "max_seam_code": 0.0,
            "failed_frames": [],
            "worst_owner_artifact_code": 0.0,
            "pass": (
                ownership["multiplicity_min"] == 1
                and ownership["multiplicity_max"] == 1
            ),
        })

    quality, cost = overlap_curve()

    report = {
        "schema": "seam-audit-v1",
        "fixture": {
            "frames": FRAMES,
            "width": W,
            "height": H,
            "pixels_per_frame": n_pixels,
            "bits": BITS,
            "code_span": CODE_SPAN,
            "quantum_normalized": 1.0 / CODE_SPAN,
            "exposure_key": KEY,
            "exposure_estimator": "g = key / arithmetic_mean_luminance",
            "tone_curve": "Reinhard: y = gL/(1+gL)",
            "seam_metric": (
                "difference of buggy and strict cross-boundary code steps"
            ),
        },
        "tolerance": {
            "max_seam_code": 1.0,
            "global_error_pixel_budget_direct_slope": error_budget,
            "formula": "E_allow = floor(WH / 255)",
            "practical_1080p_budget": math.floor(1920 * 1080 / CODE_SPAN),
        },
        "buggy_inclusive_tiers": tiers,
        "strict_half_open_tiers": strict,
        "overlap": {
            "filter": "horizontal separable box blur",
            "blur_radius": BLUR_RADIUS,
            "quality_curve": quality,
            "cost_curve": cost,
            "histogram_overlap_recommendation": (
                "Do not sum raw overlapping histograms.  Use a compute halo for "
                "filtering but an exclusive half-open ownership mask for stats."
            ),
            "quality_gate_min_halo_for_fixture": next(
                row["halo_radius_px"] for row in quality if row["quality_pass"]
            ),
            "guaranteed_halo_radius": BLUR_RADIUS,
        },
    }

    json_path = os.path.join(RESULT_DIR, "seam_report.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write("\n")

    csv_path = os.path.join(RESULT_DIR, "seam_metrics.csv")
    fields = list(metric_rows[0].keys())
    with open(csv_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(metric_rows)

    overlap_csv = os.path.join(RESULT_DIR, "overlap_curves.csv")
    with open(overlap_csv, "w", encoding="utf-8", newline="") as f:
        fields = [
            "halo_radius_px", "overlap_band_px",
            "worst_blur_seam_error_code", "quality_pass",
            "cost_K2", "cost_K4", "cost_K8",
        ]
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for q in quality:
            r = q["halo_radius_px"]
            row = {
                "halo_radius_px": r,
                "overlap_band_px": 2 * r,
                "worst_blur_seam_error_code":
                    q["worst_blur_seam_error_code"],
                "quality_pass": q["quality_pass"],
            }
            for k in CHUNK_COUNTS:
                c = next(
                    item["compute_ratio"]
                    for item in cost
                    if item["chunks"] == k and item["halo_radius_px"] == r
                )
                row[f"cost_K{k}"] = c
            writer.writerow(row)

    print(json.dumps({
        "json": json_path,
        "csv": csv_path,
        "overlap_csv": overlap_csv,
        "budget_pixels": error_budget,
        "tiers": [
            {
                "chunks": t["chunks"],
                "error_pixels": t["ownership"]["wrong_pixels"],
                "bound_code": t["worst_case_count_bound_code"],
                "worst_measured_code": t["worst_owner_artifact_code"],
                "failed_frames": t["failed_frames"],
            }
            for t in tiers
        ],
        "strict_all_pass": all(t["pass"] for t in strict),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
