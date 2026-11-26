#!/usr/bin/env python3
"""Plot the two kernel-width decision curves from bloomcast CSV output.

Reads artifacts/bloomcast_curves.csv (rows: curve,tier,gamma,spread,cR,cG,cB)
and writes artifacts/bloomcast_curves.png.

Panel A: worst color cast per lamp intensity vs relative kernel-width spread,
         with the JND envelope at 0.05.
Panel B: the trade-off curve - highlight layering (the spread knob itself)
         against cast for the mid lamp, marking the JND-limited operating
         point.

Categorical colors are the validated dataviz default slots 1-3; channels in
the supplementary channel check use line style, never hue alone. The raw
numbers accompany the figure in bloomcast_curves.csv / bloomcast_tiers.txt.
"""
import csv
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

EPS = 0.05
TIER_COLOR = {"low": "#2a78d6", "mid": "#eb6834", "high": "#1baf7a"}
TIER_LABEL = {"low": "L=2 low", "mid": "L=8 mid", "high": "L=32 high"}
CH_STYLE = {"R": "-", "G": "--", "B": ":"}
INK = "#0b0b0b"
MUTED = "#52514e"
SURFACE = "#fcfcfb"


def load(path_csv):
    rows = []
    with open(path_csv, newline="") as f:
        for row in csv.reader(f):
            rows.append({
                "tier": row[1],
                "gamma": float(row[2]),
                "spread": float(row[3]),
                "cast": [float(row[4]), float(row[5]), float(row[6])],
            })
    return rows


def main(path_csv: str, path_png: str) -> None:
    rows = load(path_csv)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.4),
                                   facecolor=SURFACE)
    for ax in (ax1, ax2):
        ax.set_facecolor(SURFACE)
        ax.grid(alpha=0.3, lw=0.7)
        ax.tick_params(colors=MUTED, labelsize=9)
        for spine in ax.spines.values():
            spine.set_color("#d8d7d2")

    # panel A: worst cast envelope per intensity tier
    for tier in ("low", "mid", "high"):
        tr = sorted((r for r in rows if r["tier"] == tier),
                    key=lambda r: r["gamma"])
        spread = [r["spread"] for r in tr]
        worst = [max(abs(c) for c in r["cast"]) for r in tr]
        ax1.plot(spread, worst, color=TIER_COLOR[tier], marker="o", ms=6,
                 lw=2, label=TIER_LABEL[tier])
        # channel split for the mid tier only, styled by line type
        if tier == "mid":
            for ci, ch in enumerate(("R", "G", "B")):
                ax1.plot(spread, [r["cast"][ci] for r in tr],
                         color=MUTED, ls=CH_STYLE[ch], lw=1.4, alpha=0.85,
                         label=f"mid channel {ch}")
    ax1.axhline(EPS, color=INK, ls="--", lw=1.2, label="JND 0.05")
    ax1.set_xlabel("relative kernel-width spread  (wmax−wmin)/wmean",
                   color=INK, fontsize=10)
    ax1.set_ylabel("halo color cast  |Hc/mean(H) − 1|", color=INK, fontsize=10)
    ax1.set_title("A. cast envelope across lamp intensities",
                  color=INK, fontsize=11)
    ax1.legend(fontsize=7.5, ncol=2, framealpha=0.9)

    # panel B: layering vs cast trade-off for the mid lamp
    tr = sorted((r for r in rows if r["tier"] == "mid"),
                key=lambda r: r["gamma"])
    spread = [r["spread"] for r in tr]
    worst = [max(abs(c) for c in r["cast"]) for r in tr]
    ax2.plot(spread, worst, color=TIER_COLOR["mid"], marker="s", ms=6, lw=2,
             label="mid lamp trade-off")
    ax2.axhline(EPS, color=INK, ls="--", lw=1.2, label="JND 0.05")
    # JND-limited point: last spread still under threshold
    ok = [(s, c) for s, c in zip(spread, worst) if c <= EPS]
    if ok:
        sx, sy = ok[-1]
        ax2.scatter([sx], [sy], s=110, facecolors="none",
                    edgecolors=INK, linewidths=1.6, zorder=5,
                    label="JND-limited width choice")
        ax2.annotate(f"spread={sx:.3f}", (sx, sy),
                     textcoords="offset points", xytext=(10, -14),
                     fontsize=8.5, color=INK)
    ax2.fill_between(spread, 0, EPS, color="#1baf7a", alpha=0.08,
                     label="acceptable cast region")
    ax2.set_xlabel("highlight layering = width spread", color=INK, fontsize=10)
    ax2.set_ylabel("worst |cast| across channels", color=INK, fontsize=10)
    ax2.set_title("B. layering vs color-accuracy trade-off",
                  color=INK, fontsize=11)
    ax2.legend(fontsize=8, framealpha=0.9)

    fig.tight_layout()
    fig.savefig(path_png, dpi=130, facecolor=SURFACE)
    print(f"wrote {path_png}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
