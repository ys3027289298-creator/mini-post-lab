#!/usr/bin/env python3
"""Render the overlap quality/cost curves as dependency-free SVG."""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSON_PATH = os.path.join(ROOT, "results", "seam_report.json")
OUT_PATH = os.path.join(ROOT, "results", "overlap_tradeoff.svg")

COLORS = {
    "blue": "#2a78d6",
    "orange": "#eb6834",
    "aqua": "#1baf7a",
    "critical": "#d03b3b",
    "ink": "#0b0b0b",
    "secondary": "#52514e",
    "muted": "#898781",
    "grid": "#e1e0d9",
    "axis": "#c3c2b7",
    "surface": "#fcfcfb",
}


def scale(domain_lo, domain_hi, range_lo, range_hi):
    return lambda v: range_lo + (v - domain_lo) / (domain_hi - domain_lo) * (
        range_hi - range_lo
    )


def text(x, y, value, size=12, fill=None, anchor="start", weight=None):
    fill = fill or COLORS["secondary"]
    weight_attr = f' font-weight="{weight}"' if weight else ""
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" '
        f'font-size="{size}" fill="{fill}"{weight_attr}>{value}</text>'
    )


def line(x1, y1, x2, y2, color, width=2, dash=None):
    dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
    return (
        f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
        f'stroke="{color}" stroke-width="{width}"{dash_attr}/>'
    )


def circle(cx, cy, color, title, r=4.5):
    return (
        f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r}" fill="{COLORS["surface"]}" '
        f'stroke="{color}" stroke-width="2"><title>{title}</title></circle>'
    )


def path_from(points, color):
    d = "M " + " L ".join(f"{x:.1f},{y:.1f}" for x, y in points)
    return f'<path d="{d}" fill="none" stroke="{color}" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>'


def main():
    with open(JSON_PATH, encoding="utf-8") as f:
        report = json.load(f)

    quality = report["overlap"]["quality_curve"]
    cost = report["overlap"]["cost_curve"]

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="500" '
        'viewBox="0 0 1000 500" role="img" aria-labelledby="title desc">',
        '<title id="title">Overlap quality and compute tradeoff</title>',
        '<desc id="desc">Left: worst seam error falls below one code at halo '
        'radius two and reaches zero at radius four. Right: compute ratio rises '
        'with halo radius for chunk counts two, four, and eight.</desc>',
        f'<rect width="1000" height="500" fill="{COLORS["surface"]}"/>',
        text(40, 34, "分块重叠量取舍：接缝质量 × 算力开销", 20,
             COLORS["ink"], weight=700),
        text(40, 56, "256×256 × 12 帧夹具；8-bit 判据为最大接缝伪影 ≤ 1 个码值",
             12, COLORS["secondary"]),
    ]

    # Left panel: quality.
    lx, ly, lw, lh = 76, 92, 390, 285
    lbottom = ly + lh
    xq = scale(0, 5, lx, lx + lw)
    yq = scale(0, 42, lbottom, ly)

    parts.append(text(lx, ly - 18, "质量曲线：盒模糊半径 R=4", 14,
                      COLORS["ink"], weight=700))
    for value in (0, 10, 20, 30, 40):
        y = yq(value)
        parts.append(line(lx, y, lx + lw, y, COLORS["grid"], 1))
        parts.append(text(lx - 10, y + 4, str(value), 11,
                          COLORS["muted"], anchor="end"))
    for r in range(6):
        x = xq(r)
        parts.append(line(x, lbottom, x, lbottom + 5, COLORS["axis"], 1))
        parts.append(text(x, lbottom + 22, str(r), 11,
                          COLORS["muted"], anchor="middle"))
    parts.append(line(lx, ly, lx, lbottom, COLORS["axis"], 1))
    parts.append(line(lx, lbottom, lx + lw, lbottom, COLORS["axis"], 1))
    parts.append(text(lx + lw / 2, lbottom + 48,
                      "halo radius r（像素；重叠带=2r）", 12,
                      COLORS["secondary"], anchor="middle"))
    parts.append(text(24, ly + lh / 2, "最坏接缝误差（码值）", 12,
                      COLORS["secondary"], anchor="middle")
                 .replace(f'<text x="24.0" y="{ly + lh / 2:.1f}"',
                          f'<text x="24" y="{ly + lh / 2:.1f}" '
                          f'transform="rotate(-90 24 {ly + lh / 2:.1f})"'))

    y_tol = yq(1)
    parts.append(line(lx, y_tol, lx + lw, y_tol, COLORS["critical"], 1.8, "6 4"))
    parts.append(text(lx + lw - 6, y_tol - 7, "1 量化级", 12,
                      COLORS["critical"], anchor="end", weight=700))

    qpoints = [(xq(row["halo_radius_px"]),
                yq(row["worst_blur_seam_error_code"])) for row in quality]
    parts.append(path_from(qpoints, COLORS["blue"]))
    for row, (x, y) in zip(quality, qpoints):
        parts.append(circle(
            x, y, COLORS["blue"],
            f"r={row['halo_radius_px']}, worst="
            f"{row['worst_blur_seam_error_code']:.3f} code, "
            f"frame {row['worst_frame']}"))
    for idx, label, dy in [
        (0, "40.54", -10), (1, "31.14", -10),
        (2, "0.62", -10), (4, "0", -10),
    ]:
        x, y = qpoints[idx]
        parts.append(text(x, y + dy, label, 11, COLORS["blue"],
                          anchor="middle", weight=700))
    parts.append(text(xq(2) + 10, yq(0.616) - 17, "实测首次达标 r=2", 11,
                      "#006300", anchor="start", weight=700))
    parts.append(text(xq(4), yq(0) + 38, "r=R=4 对任意输入精确", 11,
                      COLORS["secondary"], anchor="middle"))

    # Right panel: compute cost.
    rx, ry, rw, rh = 575, 92, 390, 285
    rbottom = ry + rh
    xr = scale(0, 5, rx, rx + rw)
    yr = scale(1.0, 1.30, rbottom, ry)

    parts.append(text(rx, ry - 18, "算力曲线：垂直分条", 14,
                      COLORS["ink"], weight=700))
    for value in (1.00, 1.10, 1.20, 1.30):
        y = yr(value)
        parts.append(line(rx, y, rx + rw, y, COLORS["grid"], 1))
        parts.append(text(rx - 10, y + 4, f"{value:.2f}×", 11,
                          COLORS["muted"], anchor="end"))
    for r in range(6):
        x = xr(r)
        parts.append(line(x, rbottom, x, rbottom + 5, COLORS["axis"], 1))
        parts.append(text(x, rbottom + 22, str(r), 11,
                          COLORS["muted"], anchor="middle"))
    parts.append(line(rx, ry, rx, rbottom, COLORS["axis"], 1))
    parts.append(line(rx, rbottom, rx + rw, rbottom, COLORS["axis"], 1))
    parts.append(text(rx + rw / 2, rbottom + 48,
                      "halo radius r（像素）", 12,
                      COLORS["secondary"], anchor="middle"))
    parts.append(text(523, ry + rh / 2, "总算力倍率", 12,
                      COLORS["secondary"], anchor="middle")
                 .replace(f'<text x="523.0" y="{ry + rh / 2:.1f}"',
                          f'<text x="523" y="{ry + rh / 2:.1f}" '
                          f'transform="rotate(-90 523 {ry + rh / 2:.1f})"'))

    colors = {2: COLORS["blue"], 4: COLORS["orange"], 8: COLORS["aqua"]}
    for k in (2, 4, 8):
        rows = [row for row in cost if row["chunks"] == k]
        points = [(xr(row["halo_radius_px"]), yr(row["compute_ratio"]))
                  for row in rows]
        parts.append(path_from(points, colors[k]))
        for row, (x, y) in zip(rows, points):
            parts.append(circle(
                x, y, colors[k],
                f"K={k}, r={row['halo_radius_px']}, "
                f"ratio={row['compute_ratio']:.4f}"))
        last = rows[-2]  # label at r=4, not r=5
        lx0 = xr(4)
        ly0 = yr(last["compute_ratio"])
        parts.append(text(lx0 + 10, ly0 + 4,
                          f"K={k}: {last['compute_ratio']:.3f}×",
                          12, colors[k], weight=700))

    x_guarantee = xr(4)
    parts.append(line(x_guarantee, ry, x_guarantee, rbottom,
                      COLORS["muted"], 1.5, "3 5"))
    parts.append(text(x_guarantee - 8, ry + 14, "r=4", 11,
                      COLORS["muted"], anchor="end", weight=700))

    # Legend.
    legend_y = 456
    legend_x = 300
    for i, (label, color) in enumerate([
        ("K=2", colors[2]), ("K=4", colors[4]), ("K=8", colors[8])
    ]):
        x = legend_x + i * 110
        parts.append(line(x, legend_y - 4, x + 24, legend_y - 4, color, 3))
        parts.append(circle(x + 12, legend_y - 4, color, label, 4))
        parts.append(text(x + 32, legend_y, label, 12,
                          COLORS["secondary"]))
    parts.append(text(500, 486,
                      "直方图统计不使用原始重叠域：halo 只服务滤波，统计仍按半开区间归属。",
                      12, COLORS["secondary"], anchor="middle"))

    parts.append("</svg>")
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    print(OUT_PATH)


if __name__ == "__main__":
    main()
