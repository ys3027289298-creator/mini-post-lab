"""Figures for the dark-detail / contrast trade-off analysis.

Run: python3 make_figures.py
Writes outputs/fig1_curves.png, fig2_frontier.png, fig3_threshold.png

Design: categorical colors are the validated dataviz default slots
(blue/orange/aqua), marks have direct labels (identity never color-alone),
single y-axis per panel, recessive grid, text in ink colors rather than series
colors.  Aqua's surface contrast is below 3:1, so every aqua series carries a
direct label (the palette rule's relief requirement).
"""

import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from metrics import (
    DARK_EDGE,
    log_curve,
    piecewise_linear_curve,
    power_curve,
)
from frontiers import (
    frontier_grid,
    pl_C_of_D,
    range_threshold,
    shadow_gain,
)

# Validated categorical palette (dataviz skill, light mode).
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"
INK, INK2, GRID = "#0b0b0b", "#52514e", "#d9d8d3"
SURFACE = "#fcfcfb"
ACCEPT = "#1baf7a"
DENY = "#e34948"

plt.rcParams.update({
    "font.size": 11,
    "axes.edgecolor": INK2,
    "axes.labelcolor": INK,
    "text.color": INK,
    "xtick.color": INK2,
    "ytick.color": INK2,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
})

OUT = os.path.join(os.path.dirname(__file__), "outputs")

# Operating-point parameters from run_analysis (D = 0.75 boundary).
GAMMA_OP = 1.17875
ALPHA_OP = 1.06964
YB_OP = 0.15
D_STAR = 0.75
C_STAR = 4.0
G_BUDGET = 40.0


def fig1_curves():
    u = np.linspace(0, 1, 500)
    fig, ax = plt.subplots(figsize=(7.6, 5.8))
    ax.plot(u, power_curve(u, GAMMA_OP), color=BLUE, lw=2,
            label=f"power  y=u^γ, γ={GAMMA_OP:.3f}")
    ax.plot(u, log_curve(u, ALPHA_OP), color=ORANGE, lw=2,
            label=f"log  α={ALPHA_OP:.3f}")
    ax.plot(u, piecewise_linear_curve(u, YB_OP), color=AQUA, lw=2,
            label=f"piecewise-linear  y_b={YB_OP:.2f}")
    ax.plot([0, 1], [0, 1], color=INK2, lw=1, ls="--", label="linear reference")
    ax.axvspan(0, DARK_EDGE, color=INK, alpha=0.05)
    ax.axvline(DARK_EDGE, color=INK2, lw=0.9, ls=":")
    ax.annotate("shadow cohort u in [0, 0.2]", xy=(0.012, 0.55), color=INK2, fontsize=9.5)
    # Mark each curve's output at u=0.2: this height defines D.
    for y, c in ((power_curve(DARK_EDGE, GAMMA_OP), BLUE),
                 (log_curve(DARK_EDGE, ALPHA_OP), ORANGE),
                 (piecewise_linear_curve(DARK_EDGE, YB_OP), AQUA)):
        ax.plot([DARK_EDGE], [y], "o", ms=7, color=c, mec=SURFACE, mew=1.5)
    ax.annotate("at u=0.2 all three curves reach y=0.15,\n"
                "i.e. the shadow cohort fills 75% of the dark zone (D = D*)",
                xy=(0.2, 0.15), xytext=(0.30, 0.045),
                color=INK2, fontsize=9.5,
                arrowprops=dict(arrowstyle="->", color=INK2, lw=0.9))
    ax.set_xlabel("normalized input position u (log-uniform scene light)")
    ax.set_ylabel("output luminance y")
    ax.set_title("Candidate tone curves at the D = 0.75 operating point")
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.legend(frameon=False, loc="upper left", bbox_to_anchor=(0.02, 0.98), fontsize=9.5)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig1_curves.png"), dpi=160)
    plt.close(fig)


def fig2_frontier():
    grid = frontier_grid()
    D = grid["D"]
    c_log = np.interp(D, grid["log_D"], grid["log_C"])

    fig, ax = plt.subplots(figsize=(8.6, 6.2))
    fig.subplots_adjust(top=0.86)
    ax.plot(D, grid["power"], color=BLUE, lw=2.2, label="power  C = 9^(ln(5/D)/ln 5)")
    ax.plot(D, pl_C_of_D(D), color=AQUA, lw=2.2,
            label="piecewise-linear  C = 8.75/D + 0.25")
    ax.plot(D, c_log, color=ORANGE, lw=2.2, label="convex log (numerically evaluated)")
    ax.axvline(D_STAR, color=INK2, lw=1, ls="--")
    ax.axhline(C_STAR, color=INK2, lw=1, ls=":")
    ax.annotate("D* = 0.75", xy=(D_STAR + 0.01, 380), color=INK2, fontsize=9.5)
    ax.annotate("dark-detail floor", xy=(D_STAR + 0.01, 260), color=INK2, fontsize=9.5)
    ax.annotate("C* = 4 (contrast floor)", xy=(0.03, 4.6), color=INK2, fontsize=9.5)

    # Shade the acceptance rectangle (D >= D*, C >= C*): a thin band along the
    # bottom since the y-axis spans orders of magnitude.
    ax.axvspan(D_STAR, 1.0, ymin=0.0, ymax=0.10, color=ACCEPT, alpha=0.08)
    ax.annotate("acceptance region (D ≥ D*, C ≥ C*)", xy=(0.76, 5.6),
                color=INK2, fontsize=9.5)

    # Operating points at D = D*, labeled in a vertical stack to the right.
    op = [
        ("power", float(np.interp(D_STAR, D, grid["power"])), BLUE, (0.79, 24.0)),
        ("PL", float(pl_C_of_D(np.array([D_STAR]))[0]), AQUA, (0.79, 7.6)),
        ("log", float(np.interp(D_STAR, D, c_log)), ORANGE, (0.79, 16.5)),
    ]
    for name, y, c, txtxy in op:
        ax.plot(D_STAR, y, "o", ms=9, color=c, mec=SURFACE, mew=1.5, zorder=5)
        ax.annotate(f"{name}: C={y:.2f}", xy=(D_STAR, y), xytext=txtxy,
                    color=INK, fontsize=9.5,
                    arrowprops=dict(arrowstyle="-", color=c, lw=0.8))

    ax.set_yscale("log")
    ax.set_xlabel("shadow-detail metric D (higher = more shadow gradation)")
    ax.set_ylabel("contrast metric C = Q0.9 / Q0.1 (log scale)")
    ax.set_title("Analytic trade-off frontiers\n"
                 "power weakly dominates: strictly better inside, equal at linear endpoint",
                 fontsize=12.5)
    ax.set_xlim(0.02, 1.0)
    ax.set_ylim(3, 700)
    ax.legend(frameon=False, loc="upper left", bbox_to_anchor=(0.30, 1.0),
              fontsize=9.5)
    fig.tight_layout(rect=(0, 0, 1, 0.90))
    fig.savefig(os.path.join(OUT, "fig2_frontier.png"), dpi=160)
    plt.close(fig)


def fig3_threshold():
    D_star = D_STAR
    G = G_BUDGET
    R_star, R_asym = range_threshold(D_star, G)
    R = np.geomspace(2, 20000, 400)
    g = np.array([shadow_gain(r, D_star) for r in R])

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12.5, 5.0))
    fig.subplots_adjust(top=0.88, wspace=0.22)

    ax1.plot(R, g, color=BLUE, lw=2.2, label="required shadow gain g(R, D*=0.75)")
    ax1.axhline(G, color=DENY, lw=1.6, ls="--", label=f"display budget G = {G:g}x")
    ax1.axvline(R_star, color=INK2, lw=1.2, ls=":")
    ax1.annotate(f"R* = {R_star:.1f}\n({np.log2(R_star):.2f} stops)",
                 xy=(R_star, G), xytext=(150, 210),
                 color=INK, fontsize=9.5,
                 arrowprops=dict(arrowstyle="->", color=INK2, lw=0.9))
    scene_labels = [
        ("bright\n6 stops", 64, (14, 13.0)),
        ("medium\n8 stops", 256, (95, 30.0)),
        ("dark\n12 stops", 4096, (5500, 70.0)),
    ]
    for name, Rv, txtxy in scene_labels:
        gv = shadow_gain(Rv, D_star)
        ok = gv <= G
        ax1.plot(Rv, gv, "o", ms=9,
                 color=ACCEPT if ok else DENY, mec=SURFACE, mew=1.5, zorder=5)
        ax1.annotate(name, xy=(Rv, gv), xytext=txtxy, color=INK2, fontsize=9)
    ax1.set_xscale("log")
    ax1.set_yscale("log")
    ax1.set_xlabel("scene dynamic range R = x_max / x_min")
    ax1.set_ylabel("shadow gain required to hold D = D*")
    ax1.set_title("Infeasibility: beyond R* the gain budget cannot be met", pad=12)
    ax1.legend(frameon=False, loc="upper left", fontsize=9.5)

    # Right panel: threshold vs budget, exact root and asymptotic form.
    Gs = np.geomspace(10, 400, 60)
    Rs = np.array([range_threshold(D_star, gv)[0] for gv in Gs])
    Ras = (5 * Gs / D_star) ** 1.25
    ax2.plot(Gs, Rs, color=BLUE, lw=2.2, label="exact root of R = A(R^0.2 − 1), A=5G/D*")
    ax2.plot(Gs, Ras, color=ORANGE, lw=1.8, ls="--",
             label="asymptotic R ≈ (5G/D*)^1.25")
    ax2.axhline(R_star, color=INK2, lw=1, ls=":")
    ax2.annotate(f"R*(G=40) = {R_star:.1f}", xy=(42, R_star),
                 xytext=(58, R_star * 0.5), color=INK2, fontsize=9.5)
    ax2.set_xscale("log")
    ax2.set_yscale("log")
    ax2.set_xlabel("shadow-gain budget G")
    ax2.set_ylabel("dynamic-range threshold R*")
    ax2.set_title("Threshold vs budget, with the closed-form estimate", pad=12)
    ax2.legend(frameon=False, loc="lower right", fontsize=9.5)

    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(os.path.join(OUT, "fig3_threshold.png"), dpi=160)
    plt.close(fig)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    fig1_curves()
    fig2_frontier()
    fig3_threshold()
    print("wrote fig1_curves.png, fig2_frontier.png, fig3_threshold.png to", OUT)
