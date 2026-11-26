#!/usr/bin/env python3
"""
两张图（数据来自 results_final.txt 的 min-of-8 统计）：
  fig1 分组柱：三档分辨率下 sep/mrt/split 的每帧实测耗时
  fig2 两条曲线：随像素数 N 变化的 耗时 与 格式自由度（可保留原生格式的目标数）
颜色取自 dataviz 已验证调色板：sep=#2a78d6, mrt=#eb6834, split=#1baf7a。
"""
import re
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

C_SEP, C_MRT, C_SPLIT = "#2a78d6", "#eb6834", "#1baf7a"
INK, MUTED, SURFACE = "#0b0b0b", "#52514e", "#fcfcfb"
plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 10,
    "text.color": INK, "axes.edgecolor": "#c9c8c2",
    "axes.labelcolor": MUTED, "xtick.color": INK, "ytick.color": INK,
    "axes.spines.top": False, "axes.spines.right": False,
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "grid.color": "#e6e5e0", "grid.linewidth": 0.8,
})

PAT = re.compile(r"mode=(\w+) res=(\d+)x(\d+).*?per_frame_ms=([\d.]+)")
rows = defaultdict(list)
for line in open("results_final.txt"):
    m = PAT.search(line)
    if m:
        mode, w, h, ms = m.groups()
        rows[(int(w), int(h), mode)].append(float(ms))
mins = {k: min(v) for k, v in rows.items()}
resos = sorted({(w, h) for (w, h, _) in mins})
labels = [f"{w}x{h}" for w, h in resos]
modes = [("sep", C_SEP, "sep (3 passes, native formats)"),
         ("mrt", C_MRT, "mrt (1 pass, all RGBA16F)"),
         ("split", C_SPLIT, "split (2 passes)")]

# ---- fig1: grouped bars ----
fig, ax = plt.subplots(figsize=(8.2, 4.6), dpi=150)
x = range(len(resos)); width = 0.26
for j, (mode, color, _) in enumerate(modes):
    vals = [mins[(w, h, mode)] for w, h in resos]
    bars = ax.bar([i + (j - 1) * width for i in x], vals, width,
                  color=color, label=modes[j][2], zorder=3)
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2, v + 0.18, f"{v:.2f}",
                ha="center", va="bottom", fontsize=8.5, color=INK)
ax.set_xticks(list(x)); ax.set_xticklabels(labels)
ax.set_ylabel("per-frame time (ms, min of 8 runs)")
ax.set_title("Post-process output strategy: measured frame time", pad=12, color=INK)
ax.yaxis.grid(True, zorder=0); ax.set_axisbelow(True)
ax.legend(frameon=False, fontsize=9, loc="upper left")
fig.tight_layout(); fig.savefig("fig1_measured.png", bbox_inches="tight")
plt.close(fig)

# ---- fig2: two-curve decision view ----
# 左轴：模型耗时曲线 T(N)；右轴用文字注解表示格式自由度，不做第二数值轴（避免双轴反模式）
import json
fit = json.load(open("model_fit.json"))
tb, cf, ta = fit["t_b_s"], fit["c_fix_s"], fit["t_alu_s"]
Ns = [int(0.25e6 * k) for k in range(1, 16)]      # 0.25M .. 3.75M px
READ, W_SEP, W_SPLIT, W_MRT = 22, 14, 12, 24
T = lambda p, w: [p * cf + n * (READ + w) * tb * 1e3 + n * ta * 1e3 for n in Ns]
fig, ax = plt.subplots(figsize=(8.2, 4.6), dpi=150)
ax.plot(Ns, T(3, W_SEP), color=C_SEP, lw=2, marker="o", ms=5,
        label="sep: time (3 passes)")
ax.plot(Ns, T(1, W_MRT), color=C_MRT, lw=2, marker="s", ms=5,
        label="mrt: time (1 pass, forced RGBA16F)")
ax.plot(Ns, T(2, W_SPLIT), color=C_SPLIT, lw=2, marker="^", ms=6,
        label="split: time (2 passes)")
for w, h in resos:
    ax.axvline(w * h, color="#c9c8c2", lw=0.9, ls="--", zorder=1)
    ax.text(w * h, ax.get_ylim()[1] * 0.02, f" {w}x{h}", rotation=90,
            va="bottom", ha="left", fontsize=8, color=MUTED)
ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v/1e6:.1f}M"))
ax.set_xlabel("frame pixels N")
ax.set_ylabel("modeled per-frame time (ms)")
ax.set_title("Time curve vs. format-freedom trade-off", pad=12, color=INK)
freedom = ("format freedom:  sep=3 native targets (RGBA8/R16F/RGBA16F)   "
           "mrt=1 common format only   split=2 native groups")
ax.text(0.0, -0.20, freedom, transform=ax.transAxes, fontsize=8.5, color=MUTED)
ax.yaxis.grid(True, zorder=0); ax.set_axisbelow(True)
ax.legend(frameon=False, fontsize=9, loc="upper left")
fig.tight_layout(); fig.savefig("fig2_curves.png", bbox_inches="tight")
plt.close(fig)
print("wrote fig1_measured.png fig2_curves.png")
