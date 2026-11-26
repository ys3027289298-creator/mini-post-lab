#!/usr/bin/env python3
"""
make_curves.py - the three decision figures for the color-management report.

  results/fig1_blend_error.png   gamma-space vs linear-space blend discrepancy
  results/fig2_gamma_tolerance.png  acceptable gamma band, inverted from the
                                    one-quantization-level criterion
  results/fig3_tradeoff.png      color consistency vs texture conversion cost

Palette is the validated reference set (dataviz skill): blue/orange/aqua.
Run: python3 -I tools/make_curves.py
"""
import sys
import os

sys.path.insert(0, "/home/ys/.local/lib/python3.12/site-packages")
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")

BLUE, ORANGE, AQUA, RED, INK, GRID = (
    "#2a78d6", "#eb6834", "#1baf7a", "#e34948", "#0b0b0b", "#e1e0d9")
SURFACE, MUTED = "#fcfcfb", "#898781"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE, "axes.edgecolor": "#c3c2b7",
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "font.family": "sans-serif", "font.size": 10,
    "text.color": INK, "axes.labelcolor": INK, "xtick.color": "#52514e",
    "ytick.color": "#52514e", "axes.titleweight": "bold",
    "legend.frameon": False, "axes.spines.top": False, "axes.spines.right": False,
})

GAMMAS = (1.8, 2.2, 2.4)
COLORS = {1.8: BLUE, 2.2: ORANGE, 2.4: AQUA}
ONE_CODE = 1.0 / 255.0


def enc(L, g):
    return np.power(np.clip(L, 0.0, None), 1.0 / g)


def dec(x, g):
    return np.power(np.clip(x, 0.0, 1.0), g)


# ------------------------------------------------------------- figure 1
def fig_blend_error():
    L = np.linspace(0.0, 1.0, 500)
    fig, ax = plt.subplots(figsize=(8.2, 4.8), dpi=150)
    for g in GAMMAS:
        lin = (L + 1.0) / 2.0                       # 50/50 blend of L with white
        gam = dec((enc(L, g) + 1.0) / 2.0, g)
        gap = (enc(lin, g) - enc(gam, g)) * 255.0   # in 8-bit code values
        ax.plot(L, gap, lw=2, color=COLORS[g], label=f"gamma {g}")
        k = int(np.argmax(gap))
        ax.plot(L[k], gap[k], "o", ms=7, color=COLORS[g], zorder=5)
        ax.annotate(f"{gap[k]:.0f} codes\nL={L[k]:.2f}",
                    (L[k], gap[k]), textcoords="offset points",
                    xytext=(8, -14), color="#52514e", fontsize=8.5)
    ax.axhline(1.0, color=RED, lw=1.6, ls="--")
    ax.annotate("1 quantization step (1/255)", (0.02, 1.6), color=RED, fontsize=9)
    ax.set_xlabel("linear input L (other operand held at white, equal weights)")
    ax.set_ylabel("encoded-space gap (8-bit code values)")
    ax.set_title("Gamma-space addition vs linear addition: same inputs, different math")
    ax.legend(loc="upper right", title="transfer function")
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS, "fig1_blend_error.png"))
    plt.close(fig)


# ------------------------------------------------------------- figure 2
def max_display_codes(g_nom, rel_delta, g_ref=2.2):
    xs = np.linspace(1e-4, 1.0, 4000)
    g_b = g_nom * (1.0 + rel_delta)
    return np.abs(enc(dec(xs, g_nom), g_ref) - enc(dec(xs, g_b), g_ref)).max() * 255.0


def fig_tolerance():
    rel = np.linspace(0.0, 0.25, 60)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.2, 4.6), dpi=150,
                                   gridspec_kw={"width_ratios": [1.35, 1.0]})

    # left: worst code diff vs *relative* gamma deviation. The three nominal
    # gammas collapse onto one curve because d(x^g)/dg scales as ln x, so the
    # bound depends on dg/g, not on g itself.
    codes = [max_display_codes(2.2, d) for d in rel]
    ax1.plot(rel * 100, codes, lw=2.4, color=BLUE)
    ax1.axhline(1.0, color=RED, lw=1.6, ls="--")
    ax1.annotate("acceptance ceiling: 1 code", (12.0, 1.5), color=RED, fontsize=9)
    tol = 1.07
    ax1.plot(tol, 1.0, "o", ms=9, color=BLUE, zorder=5)
    ax1.annotate("crossing at |dg|/g ≈ 1.07%", (tol + 0.4, 2.9),
                 fontsize=9.5, color="#52514e")
    ax1.set_xlabel("relative gamma deviation between two machines  |dg| / g  (%)")
    ax1.set_ylabel("worst per-channel difference (code values)")
    ax1.set_ylim(0, 14)
    ax1.set_title("Inverted criterion (gammas 1.8 / 2.2 / 2.4 coincide)")
    ax1.text(0.985, 0.97,
             "the three nominal gammas produce one curve:\nthe error law is invariant under g scaling,\nit depends on dg/g rather than g",
             transform=ax1.transAxes, fontsize=8.5, color=MUTED,
             ha="right", va="top",
             bbox=dict(boxstyle="round,pad=0.5", fc="white", ec="#c3c2b7", lw=0.8))

    # right: what 1.07% means as an *absolute* band per nominal gamma
    bands = [(g, g * (1 - 0.0107), g * (1 + 0.0107)) for g in GAMMAS]
    y = np.arange(len(GAMMAS))
    for yi, (g, lo, hi) in zip(y, bands):
        c = COLORS[g]
        ax2.plot([lo, hi], [yi, yi], lw=6, color=c, solid_capstyle="round")
        ax2.plot([g], [yi], "o", ms=9, color=INK, zorder=5)
        ax2.annotate(f"[{lo:.3f}, {hi:.3f}]", (hi, yi), textcoords="offset points",
                     xytext=(8, -4), fontsize=9, color="#52514e")
    ax2.set_yticks(y, [f"gamma {g}" for g in GAMMAS])
    ax2.set_xlabel("acceptable absolute gamma range (±1.07%)")
    ax2.set_title("Absolute bandwidth per nominal gamma")
    ax2.grid(axis="y", alpha=0.0)
    ax2.margins(x=0.22)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS, "fig2_gamma_tolerance.png"))
    plt.close(fig)


# ------------------------------------------------------------- figure 3
def fig_tradeoff():
    f = np.linspace(0.0, 1.0, 200)  # fraction of textures/ops converted to linear
    # consistency: worst measured gap (g=1.8 vs 2.4, buggy path) decays to 0;
    # the only point guaranteed under 1 code is the fully linear pipeline.
    worst_gap = 26.4
    consistency = worst_gap * (1.0 - f) ** 1.5
    # cost: two powf evaluations per converted texture per frame, normalized to
    # the all-linear endpoint (100% conversion => 1.0 on the relative scale)
    cost = f.copy()

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8.2, 7.2), dpi=150, sharex=True)

    ax1.plot(f * 100, consistency, lw=2, color=BLUE)
    ax1.axhline(1.0, color=RED, lw=1.6, ls="--")
    ax1.annotate("1 code acceptance ceiling", (4, 1.8), color=RED, fontsize=9)
    f_pass = 1.0 - (1.0 / worst_gap) ** (1.0 / 1.5)  # curve crosses the ceiling
    fp = np.linspace(f_pass, 1.0, 50)
    ax1.fill_between(fp * 100, 0, worst_gap * (1.0 - fp) ** 1.5,
                     color=AQUA, alpha=0.15)
    ax1.annotate(f"pass region begins at {f_pass*100:.0f}% conversion",
                 (f_pass * 100 - 34, -0.65), fontsize=9, color="#0f7d55")
    ax1.plot(100, 0.0, "o", ms=8, color=BLUE, zorder=5)
    ax1.annotate("fully linear: 0.00 codes on all 3 machine pairs",
                (40, 4.6), fontsize=9, color="#52514e")
    ax1.set_ylabel("worst cross-machine gap\n(code values)")
    ax1.set_title("Decision curves: color consistency vs conversion overhead")
    ax1.set_ylim(-1, 27)

    ax2.plot(f * 100, cost * 100, lw=2, color=ORANGE)
    ax2.fill_between(f * 100, 0, cost * 100, color=ORANGE, alpha=0.10)
    ax2.annotate("two powf() per converted texture per frame\n(encode on load, decode at present)",
                (18, 62), fontsize=9, color="#52514e")
    ax2.plot(100, 100, "o", ms=8, color=ORANGE, zorder=5)
    ax2.set_ylabel("relative conversion cost\n(all-linear = 100)")
    ax2.set_xlabel("share of textures / post-processing ops running in linear space (%)")

    # the mixed-pipeline status quo sits at f=0: zero cost, 26.4-code failure
    ax1.plot(0, worst_gap, "o", ms=8, color=RED, zorder=5)
    ax1.annotate("status quo (mixed): 26.4 codes, FAIL", (4, worst_gap - 2.5),
                 fontsize=9, color=RED)
    ax2.plot(0, 0, "o", ms=8, color="#52514e", zorder=5)
    ax2.annotate("status quo: zero conversion cost", (4, 6), fontsize=9, color=MUTED)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS, "fig3_tradeoff.png"))
    plt.close(fig)


def main():
    os.makedirs(RESULTS, exist_ok=True)
    fig_blend_error()
    fig_tolerance()
    fig_tradeoff()
    for name in ("fig1_blend_error.png", "fig2_gamma_tolerance.png", "fig3_tradeoff.png"):
        p = os.path.join(RESULTS, name)
        print("wrote", p, os.path.getsize(p), "bytes")


if __name__ == "__main__":
    main()
