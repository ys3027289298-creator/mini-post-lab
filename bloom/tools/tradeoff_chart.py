#!/usr/bin/env python3
"""Render the bloom consistency/cost trade-off from calibration JSON."""

from __future__ import annotations

import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

plt.rcParams["font.sans-serif"] = ["Noto Sans CJK SC", "WenQuanYi Zen Hei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

ROOT = Path(__file__).resolve().parents[2]
JSON_PATH = ROOT / "artifacts" / "bloom_resolution_results.json"
OUT_PATH = ROOT / "artifacts" / "bloom_tradeoff.png"

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
SECONDARY = "#52514e"
GRID = "#e1e0d9"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
AQUA = "#1baf7a"
CRITICAL = "#d03b3b"


def main() -> None:
    data = json.loads(JSON_PATH.read_text(encoding="utf-8"))
    rows = data["rows"]
    labels = [f'{r["width"]}×{r["height"]}' for r in rows]
    target = data["params"]["coverage_target"]
    base_ratio = [r["baseline_area_fraction"] / target for r in rows]
    corr_ratio = [r["corrected_area_fraction"] / target for r in rows]

    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(11.2, 4.6), dpi=170)
    fig.patch.set_facecolor(SURFACE)
    for ax in (ax0, ax1):
        ax.set_facecolor(SURFACE)
        ax.tick_params(colors=SECONDARY, labelsize=9)
        for spine in ax.spines.values():
            spine.set_color(GRID)
        ax.grid(axis="y", color=GRID, linewidth=0.8)
        ax.set_axisbelow(True)

    x = np.arange(len(labels))
    width = 0.36
    bars0 = ax0.bar(x - width / 2, base_ratio, width, label="固定 τ=1", color=ORANGE)
    bars1 = ax0.bar(x + width / 2, corr_ratio, width, label="面积反解 τ(h)", color=BLUE)
    ax0.axhspan(0.9, 1.1, color=AQUA, alpha=0.12, zorder=0)
    ax0.axhline(1.0, color=SECONDARY, linewidth=1.0)
    ax0.set_xticks(x, labels)
    ax0.set_ylabel("覆盖面积 / 目标面积", color=INK, fontsize=10)
    ax0.set_title("一致性：绿色带为 ±10%", color=INK, fontsize=12, pad=10)
    ax0.set_ylim(0, 1.56)
    ax0.legend(frameon=False, fontsize=9, labelcolor=INK, loc="upper right")
    for bars in (bars0, bars1):
        for bar in bars:
            height = bar.get_height()
            ax0.annotate(
                f"{height:.3f}",
                (bar.get_x() + bar.get_width() / 2, height),
                xytext=(0, 3),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8,
                color=SECONDARY,
            )
    ax0.text(
        0.02,
        0.97,
        f'固定阈值最大档差 {100*data["baseline_max_relative_spread_vs_smaller"]:.1f}%\n'
        f'修正后最大档差 {100*data["corrected_max_relative_spread_vs_smaller"]:.1f}%',
        transform=ax0.transAxes,
        fontsize=9,
        color=INK,
        va="top",
        bbox={"boxstyle": "round,pad=0.45", "facecolor": SURFACE, "edgecolor": GRID},
    )

    switches = np.arange(0, 21)
    newton_per_switch = data["cost_model_720p_equivalent_evaluations"][
        "newton_per_switch_worst_tier"
    ]
    # Three presets, three coverage evaluations per preset for a central
    # derivative + corrected verification, in 720p-equivalent passes.
    preset_cost = 3.0 * sum(data["cost_model_720p_equivalent_evaluations"]["per_resolution"].values())
    ax1.plot(switches, np.zeros_like(switches), color=SECONDARY, linewidth=2.2, label="固定阈值：0")
    ax1.plot(
        switches,
        newton_per_switch * switches,
        color=ORANGE,
        linewidth=2.2,
        label="每次 Newton 重算",
    )
    ax1.axhline(
        preset_cost,
        color=BLUE,
        linewidth=2.2,
        linestyle="--",
        label="三档预标定/缓存",
    )
    break_even = int(np.ceil(preset_cost / newton_per_switch))
    ax1.axvline(break_even, color=CRITICAL, linewidth=1.2, linestyle=":")
    ax1.annotate(
        f"约第 {break_even} 次切换回本",
        xy=(break_even, preset_cost),
        xytext=(break_even + 1.2, preset_cost + 24),
        arrowprops={"arrowstyle": "->", "color": SECONDARY, "lw": 1.0},
        fontsize=9,
        color=INK,
    )
    ax1.set_xlabel("分辨率切换次数", color=INK, fontsize=10)
    ax1.set_ylabel("累计开销（720p 等效整帧遍数）", color=INK, fontsize=10)
    ax1.set_title("重算开销曲线", color=INK, fontsize=12, pad=10)
    ax1.set_xlim(-0.4, 20.4)
    ax1.set_ylim(0, max(newton_per_switch * 20, preset_cost) * 1.08)
    ax1.legend(frameon=False, fontsize=9, labelcolor=INK, loc="upper left")

    fig.tight_layout(w_pad=2.2)
    fig.savefig(OUT_PATH, facecolor=fig.get_facecolor(), bbox_inches="tight")
    print(OUT_PATH)


if __name__ == "__main__":
    main()
