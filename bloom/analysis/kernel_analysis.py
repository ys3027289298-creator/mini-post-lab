"""
kernel_analysis.py — bloom 稀疏高斯核的负权重与噪声放大分析

模型
----
目标：标准差 sigma、支撑半径 R 的高斯模糊，用 N=2m+1 个对称采样点实现，
点间距 dx = 2R/(N-1)，间距比 q = dx/sigma。

两种离散近似：
  1. moment  : 矩匹配（高斯求积型近似）。由前 m+1 个偶阶矩方程解权重，
               保证核的方差/峰度等与目标高斯一致；是“少量采样点拟合大半径
               高斯”的标准紧凑核构造。
  2. sampled : 直接在采样点上取高斯值并归一化（永远正权重，作为安全基线）。

噪声模型
--------
暗部噪声 n 为零均值、方差 sigma_n^2 的白噪声，卷积 y = sum_j w_j n_j：
  Var(y) = sigma_n^2 * sum_j w_j^2
  1D 噪声增益 G1 = sqrt(sum_j w_j^2)
  可分离 2D（两个相同 1D 核）：G2 = G1^2
权重和恒为 1 时，全正权重 => G1 <= 1（模糊只压噪声）；
出现负权重且 L2 能量超过 1 时 G1 > 1，噪声被反相叠加放大。

判据：可接受倍数 alpha = 1.10（即 G1 <= 1.10，2D G2 <= 1.21）。
"""
import csv
import math
import os

import numpy as np

OUT = os.path.dirname(os.path.abspath(__file__))
RNG = np.random.default_rng(20261127)
ALPHA = 1.10

# dataviz 参考调色板（light），已通过 validate_palette
BLUE = "#2a78d6"
ORANGE = "#eb6834"
RED = "#d03b3b"
GREEN = "#008300"
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"


# ---------------------------------------------------------------- 核构造
def moment_matched_kernel(N, R, sigma):
    """对称矩匹配核。返回 (positions, weights, dx)。"""
    assert N % 2 == 1 and N >= 3
    m = N // 2
    dx = 2.0 * R / (N - 1)
    j = np.arange(m + 1)
    x = j * dx
    k = np.arange(m + 1)
    # mu_2k = (2k-1)!! sigma^(2k)
    dbl = np.array([math.prod(range(1, 2 * kk, 2)) if kk > 0 else 1 for kk in k], float)
    mu = dbl * sigma ** (2 * k)
    A = np.where(j == 0, 1.0, 2.0)[None, :] * x[None, :] ** (2 * k[:, None])
    wh = np.linalg.solve(A, mu)
    pos = np.concatenate([-x[:0:-1], x])
    w = np.concatenate([wh[:0:-1], wh])
    return pos, w, dx


def sampled_gaussian_kernel(N, R, sigma):
    """直接采样归一化高斯，永远正权重。"""
    m = N // 2
    dx = 2.0 * R / (N - 1)
    pos = (np.arange(N) - m) * dx
    w = np.exp(-0.5 * (pos / sigma) ** 2)
    w /= w.sum()
    return pos, w, dx


# ---------------------------------------------------------------- 指标
def kernel_metrics(w):
    total = float(w.sum())
    g1 = float(np.sqrt((w ** 2).sum()))
    neg_mass = float(-w[w < 0].sum())
    n_neg = int((w < -1e-12).sum())
    return total, g1, g1 ** 2, neg_mass, n_neg


def fit_error(pos, w, R, sigma, dx):
    """核作为分段常数像素密度对目标高斯密度的归一化 RMSE。

    每次采样等价于在 [x_j-dx/2, x_j+dx/2] 上取常密度 w_j/dx 的像素盒，
    与连续高斯密度 g(x) 在细网格上比较；随采样点增多单调下降。
    """
    grid = np.linspace(-R, R, 1601)
    target = np.exp(-0.5 * (grid / sigma) ** 2) / (sigma * math.sqrt(2 * math.pi))
    approx = np.zeros_like(grid)
    for p, ww in zip(pos, w):
        approx += (ww / dx) * ((grid >= p - dx / 2) & (grid < p + dx / 2))
    return float(np.sqrt(((approx - target) ** 2).mean()) / target.max())


def monte_carlo_gain_1d(w, trials=60000):
    """白噪声卷积的实测 1D 增益，验证 G1 = sqrt(sum w^2)。"""
    N = len(w)
    noise = RNG.standard_normal((trials, N))
    return float(np.std(noise @ w))


def monte_carlo_gain_2d(w, trials=40000, n_sites=32):
    """可分离两次 1D 卷积的实测 2D 增益，验证 G2 = sum w^2 = G1^2。"""
    N = len(w)
    pad = N // 2
    n = n_sites + 2 * pad
    img = RNG.standard_normal((trials, n, n))
    # 水平 pass：只保留中心 n_sites 列
    tmp = np.zeros((trials, n, n_sites))
    for j, wj in enumerate(w):
        tmp += wj * img[:, :, j:j + n_sites]
    # 垂直 pass：只统计中心输出点 (row=center, col=center)
    r0 = pad
    c = n_sites // 2
    out = sum(wj * tmp[:, r0 + (j - pad), c] for j, wj in enumerate(w))
    return float(np.std(out))


# ---------------------------------------------------------------- 1) 无量纲扫描
def dimensionless_sweep():
    qs = np.linspace(0.30, 3.0, 541)
    Ns = [3, 5, 7, 9, 11, 13, 15]
    rows = []
    windows = []
    for N in Ns:
        g1s, negs = [], []
        for q in qs:
            _, w, _ = moment_matched_kernel(N, R=q * (N - 1) / 2.0, sigma=1.0)
            _, g1, _, neg, _ = kernel_metrics(w)
            g1s.append(g1)
            negs.append(neg)
            rows.append({"N": N, "q": round(q, 4), "G1": g1, "neg_mass": neg})
        g1s, negs = np.array(g1s), np.array(negs)
        i1 = np.argmin(abs(qs - 1.0))
        # 数值求解在近正权重区产生 ~1e-13 量级伪负值；
        # 门限 1e-6 既剔除伪负值，又不吞掉真实负肩（真实负质量 >= 1e-4）
        pos = negs <= 1e-6
        lo = i1
        while lo > 0 and pos[lo - 1]:
            lo -= 1
        hi = i1
        while hi < len(qs) - 1 and pos[hi + 1]:
            hi += 1
        bad = np.where(g1s > ALPHA)[0]
        q_budget = float(qs[bad].max()) if len(bad) else np.nan
        windows.append({
            "N": N,
            "q_lo_positive": round(float(qs[lo]), 3),
            "q_hi_positive": round(float(qs[hi]), 3),
            "q_dense_budget_edge": round(q_budget, 3),
            "G1_max_sparse": round(float(g1s[qs >= qs[hi]].max()), 4),
        })
    with open(os.path.join(OUT, "q_sweep.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=["N", "q", "G1", "neg_mass"])
        wr.writeheader()
        wr.writerows(rows)
    with open(os.path.join(OUT, "thresholds.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(windows[0].keys()))
        wr.writeheader()
        wr.writerows(windows)
    return qs, rows, windows


# ---------------------------------------------------------------- 2) 三档半径
TIERS = [
    # name, R(px), sigma(px) —— 同一张半分辨率 bloom 缓冲上的窄/中/宽三档
    ("narrow  R=8  sigma=5", 8.0, 5.0),
    ("medium  R=16 sigma=6", 16.0, 6.0),
    ("wide    R=32 sigma=8", 32.0, 8.0),
]
TAP_GRID = list(range(3, 18, 2))  # 3..17 奇数


def tier_sweep():
    report = []
    for name, R, sigma in TIERS:
        per_n = []
        for N in TAP_GRID:
            pos, w, dx = moment_matched_kernel(N, R, sigma)
            total, g1, g2, neg, n_neg = kernel_metrics(w)
            err = fit_error(pos, w, R, sigma, dx)
            per_n.append({
                "tier": name, "R": R, "sigma": sigma, "N": N, "dx": dx,
                "q": dx / sigma, "sum": total, "G1": g1, "G2": g2,
                "neg_mass": neg, "n_neg_taps": n_neg, "fit_err": err,
                "pass": g1 <= ALPHA,
            })
        report.extend(per_n)
        # 直接采样基线在同一档位下的最差（最少采样点）与 9-tap 表现
        for N in (3, 9):
            pos, w, dx = sampled_gaussian_kernel(N, R, sigma)
            _, g1, g2, neg, _ = kernel_metrics(w)
            report.append({
                "tier": name + " [sampled baseline]", "R": R, "sigma": sigma,
                "N": N, "dx": dx, "q": dx / sigma, "sum": float(w.sum()),
                "G1": g1, "G2": g2, "neg_mass": neg, "n_neg_taps": 0,
                "fit_err": fit_error(pos, w, R, sigma, dx), "pass": g1 <= ALPHA,
            })
    with open(os.path.join(OUT, "tier_sweep.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(report[0].keys()))
        wr.writeheader()
        wr.writerows(report)
    return report


def summarize(report):
    print("=" * 96)
    print(f"判据: 1D 噪声增益 G1 = sqrt(sum w^2) <= {ALPHA}  (2D 可分离 G2 = G1^2 <= {ALPHA**2:.3f})")
    print("=" * 96)
    for name, R, sigma in TIERS:
        rows = [r for r in report if r["tier"] == name]
        feasible = [r for r in rows if r["pass"]]
        print(f"\n### {name}  (R/sigma = {R/sigma:.3f})")
        print(f"{'N':>3} {'dx':>7} {'q=dx/sig':>9} {'sum':>8} {'G1':>9} {'G2':>8} "
              f"{'neg_mass':>10} {'#neg':>5} {'fit_err':>9}  verdict")
        for r in rows:
            tag = "PASS" if r["pass"] else "FAIL"
            print(f"{r['N']:>3} {r['dx']:>7.3f} {r['q']:>9.3f} {r['sum']:>8.4f} "
                  f"{r['G1']:>9.4f} {r['G2']:>8.4f} {r['neg_mass']:>10.5f} "
                  f"{r['n_neg_taps']:>5} {r['fit_err']:>9.5f}  {tag}")
        if feasible:
            n_min = min(r["N"] for r in feasible)
            n_max = max(r["N"] for r in feasible)
            # 拐点：达标窗口内，再增加一个采样点带来的相对误差改善首次 < 10%
            pick = feasible[0]
            for a, b in zip(feasible, feasible[1:]):
                rel = (a["fit_err"] - b["fit_err"]) / max(a["fit_err"], 1e-12)
                pick = b
                if rel < 0.10:
                    break
            print(f"--> 达标采样点数窗口: N in [{n_min}, {n_max}]（奇数），最少 {n_min} tap；"
                  f"推荐 N={pick['N']}（形状误差 {pick['fit_err']:.4f}，每像素 2×{pick['N']} 次采样）")
        else:
            print("--> 无任何 N 达标（该档 R/sigma 过小，支撑截断使矩方程病态）")
        fails = [r for r in rows if not r["pass"]]
        for r in fails:
            print(f"    超界: N={r['N']:>2}  R={r['R']:.0f}px  sigma={r['sigma']:.0f}px  "
                  f"G1={r['G1']:.3f}  负权重总和={r['neg_mass']:.5f} ({r['n_neg_taps']} 个负 tap)")
    print("\n### 直接采样归一化高斯基线（永远正权重）")
    base = [r for r in report if "sampled baseline" in r["tier"]]
    for r in base:
        print(f"{r['tier']:<34} N={r['N']:>2}  G1={r['G1']:.4f}  neg_mass={r['neg_mass']:.4f}")


# ---------------------------------------------------------------- 3) Monte Carlo 验证
def mc_check():
    print("\n" + "=" * 96)
    print("Monte Carlo 白噪声验证（理论 G1=sqrt(sum w^2), G2=G1^2）")
    print("=" * 96)
    cases = [
        ("正权重采样核", *sampled_gaussian_kernel(9, 16.0, 6.0)),
        ("矩匹配-安全区", *moment_matched_kernel(9, 16.0, 6.0)),
        ("矩匹配-密集截断病态", *moment_matched_kernel(9, 8.0, 5.0)),
        ("矩匹配-稀疏负肩", *moment_matched_kernel(3, 32.0, 8.0)),
    ]
    mc_rows = []
    for label, pos, w, dx in cases:
        _, g1, g2, neg, _ = kernel_metrics(w)
        m1 = monte_carlo_gain_1d(w)
        m2 = monte_carlo_gain_2d(w)
        mc_rows.append({"case": label, "G1_theory": g1, "G1_mc": m1,
                        "G2_theory": g2, "G2_mc": m2, "neg_mass": neg})
        print(f"{label:<22} G1 理论={g1:7.4f} 实测={m1:7.4f} | "
              f"G2 理论={g2:7.4f} 实测={m2:7.4f} | neg={neg:.5f}")
    with open(os.path.join(OUT, "monte_carlo.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(mc_rows[0].keys()))
        wr.writeheader()
        wr.writerows(mc_rows)


# ---------------------------------------------------------------- 4) 绘图
def _style(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(True, color=GRID, lw=0.8)
    for s in ax.spines.values():
        s.set_color(BASELINE)
    ax.tick_params(colors=MUTED, labelsize=9)
    ax.xaxis.label.set_color(INK)
    ax.yaxis.label.set_color(INK)
    ax.title.set_color(INK)


def plot_all(qs, sweep_rows, windows, report):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.font_manager as fm
    import matplotlib.pyplot as plt

    for path in (os.path.expanduser("~/.fonts/NotoSansCJKsc-Regular.otf"),
                 os.path.expanduser("~/.local/share/fonts/wqy-zenhei.ttc")):
        if os.path.exists(path) and not any(f.fname == path for f in fm.fontManager.ttflist):
            fm.fontManager.addfont(path)
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["Noto Sans CJK SC", "WenQuanYi Zen Hei",
                            "Noto Sans", "Liberation Sans", "DejaVu Sans"],
        "axes.unicode_minus": False,
        "font.size": 10,
        "axes.edgecolor": BASELINE,
        "figure.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
    })

    # 图1：G1 vs q 普适曲线 + 噪声预算线
    fig, ax = plt.subplots(figsize=(8.2, 5.2), dpi=150)
    for N in (3, 5, 7, 9, 13):
        rs = [r for r in sweep_rows if r["N"] == N]
        ax.plot([r["q"] for r in rs], [r["G1"] for r in rs],
                lw=2, label=f"N={N}", color=BLUE if N == 9 else MUTED,
                alpha=1.0 if N == 9 else 0.55 + 0.06 * (N % 5))
    ax.axhline(ALPHA, color=RED, lw=1.8, ls="--", label=f"噪声预算 G1={ALPHA}")
    ax.axhline(1.0, color=GREEN, lw=1.4, ls=":", label="G1=1（全正权重上界）")
    ax.axvspan(0.3, 0.8, color=RED, alpha=0.07)
    ax.axvspan(1.25, 3.0, color=ORANGE, alpha=0.07)
    ax.set_xlabel("间距比 q = dx / σ")
    ax.set_ylabel("1D 噪声增益 G1 = √(Σ w_j²)")
    ax.set_title("矩匹配离散高斯：噪声增益只取决于间距比 q")
    ax.set_ylim(0, 3.2)
    ax.text(0.55, 2.95, "密集截断区\nRunge 型病态", color=RED,
            ha="center", va="top", fontsize=9)
    ax.text(2.1, 2.95, "稀疏区\n外侧负肩（G1 仍<1）", color=ORANGE,
            ha="center", va="top", fontsize=9)
    ax.legend(frameon=False, fontsize=9, ncol=2, loc="center right",
              bbox_to_anchor=(0.995, 0.62))
    _style(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig1_gain_vs_q.png"))
    plt.close(fig)

    # 图2：负权重总量 vs q
    fig, ax = plt.subplots(figsize=(8.2, 4.6), dpi=150)
    line_colors = {3: BLUE, 5: ORANGE, 7: MUTED, 9: RED, 13: "#6250d6"}
    for N in (3, 5, 7, 9, 13):
        rs = [r for r in sweep_rows if r["N"] == N]
        ys = [max(r["neg_mass"], 1e-5) for r in rs]
        ax.plot([r["q"] for r in rs], ys, lw=2, label=f"N={N}",
                color=line_colors[N])
    ax.set_yscale("log")
    ax.set_xlabel("间距比 q = dx / σ")
    ax.set_ylabel("负权重总和  −Σ min(w_j, 0)（对数）")
    ax.set_title("负权重出现的两个区间：q 过小（交替大权重）与 q 过大（外侧负肩）")
    ax.legend(frameon=False, fontsize=9)
    _style(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig2_negative_mass.png"))
    plt.close(fig)

    # 图3：三档半径 —— G1 与每像素开销两条曲线定采样数
    cap = 2.5  # 纵轴截断：远超预算的病态核在轴顶标注真实 G1
    fig, axes = plt.subplots(1, 3, figsize=(12.5, 4.6), dpi=150, sharey=False)
    for ax, (name, R, sigma) in zip(axes, TIERS):
        rows = [r for r in report if r["tier"] == name]
        Ns = [r["N"] for r in rows]
        g1_raw = [r["G1"] for r in rows]
        g1 = [min(g, cap) for g in g1_raw]
        err = [r["fit_err"] for r in rows]
        col = [BLUE if r["pass"] else RED for r in rows]
        ax.plot(Ns, g1, color=MUTED, lw=1.2, zorder=1)
        ax.scatter(Ns, g1, c=col, s=46, zorder=2)
        for idx, (x, gr) in enumerate(zip(Ns, g1_raw)):
            if gr > cap:
                ax.annotate(f"{gr:.2g}", (x, cap), textcoords="offset points",
                            xytext=(0, 4 if idx % 2 == 0 else 14),
                            ha="center", fontsize=8, color=RED)
        ax.axhline(ALPHA, color=RED, lw=1.5, ls="--")
        ax.text(Ns[0] + 0.4, ALPHA + 0.05, f"预算 G1={ALPHA}", color=RED, fontsize=8.5)
        ax.set_xlabel("采样点数 N（每 1D pass）")
        ax.set_ylabel("噪声增益 G1（截断显示）", color=BLUE)
        ax.set_ylim(0, cap * 1.12)
        ax2 = ax.twinx()
        ax2.plot(Ns, [max(e, 1e-3) for e in err], color=ORANGE, lw=2, marker="o", ms=5)
        ax2.set_yscale("log")
        ax2.set_ylabel("形状拟合误差（归一化 RMSE，对数）", color=ORANGE)
        ax2.tick_params(colors=MUTED, labelsize=9)
        for s in ax2.spines.values():
            s.set_color(BASELINE)
        ax.set_title(f"{name}\n每像素开销 = 2×N 次采样（可分离）", fontsize=10)
        _style(ax)
    fig.suptitle("三档半径：蓝点达标、红点超预算（红数字为真实 G1）；橙线为核形状精度——拐点即采样数取舍点",
                 fontsize=11, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(os.path.join(OUT, "fig3_tier_tradeoff.png"))
    plt.close(fig)

    # 图4：核形状对比（narrow 档 N=9 的病态核 vs 安全核 vs 正权重基线）
    fig, axes = plt.subplots(1, 3, figsize=(12.5, 4.2), dpi=150)
    show = [
        ("narrow R=8 σ=5, N=9（密集截断，FAIL）", moment_matched_kernel(9, 8, 5)),
        ("medium R=16 σ=6, N=9（安全区，PASS）", moment_matched_kernel(9, 16, 6)),
        ("同档位直接采样基线（全正）", sampled_gaussian_kernel(9, 8, 5)),
    ]
    for ax, (title, (pos, w, dx)) in zip(axes, show):
        colors = [RED if x < 0 else BLUE for x in w]
        ax.bar(pos, w, width=0.6 * dx, color=colors, edgecolor=SURFACE, lw=0.8)
        ax.axhline(0, color=BASELINE, lw=1)
        _, g1, _, neg, nneg = kernel_metrics(w)
        ax.set_title(f"{title}\nG1={g1:.3f} 负权重和={neg:.4f} ({nneg} tap)", fontsize=9.5)
        ax.set_xlabel("采样位置 x (px)")
        ax.set_ylabel("权重 w_j （Σw=1）")
        _style(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig4_kernel_shapes.png"))
    plt.close(fig)


def main():
    qs, sweep_rows, windows = dimensionless_sweep()
    print("无量纲阈值窗口（矩匹配核，sigma=1）：")
    for w_ in windows:
        print(f"  N={w_['N']:>2}: 正权重 q 窗口 [{w_['q_lo_positive']}, "
              f"{w_['q_hi_positive']}]  G1=1.1 密集侧边 q={w_['q_dense_budget_edge']}  "
              f"稀疏侧最大 G1={w_['G1_max_sparse']}")
    report = tier_sweep()
    summarize(report)
    mc_check()
    plot_all(qs, sweep_rows, windows, report)
    print("\n输出：", OUT)
    for f in sorted(os.listdir(OUT)):
        if f.endswith((".csv", ".png")):
            print("  ", f)


if __name__ == "__main__":
    main()
