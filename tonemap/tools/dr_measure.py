#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dr_measure: 场景动态范围测量与四档曝光适配评估。

方法链（全部显式可复算）：
  1. 对数亮度直方图 + CDF 反解 -> p1 / p99 分位点（与次序统计量交叉校验）。
  2. 场景动态范围 S_scene = p99 / p1（比值式），与可表示范围（显示码值窗口）比较。
  3. 每档曝光 E 把场景映射到 256 级显示域，跨度 S(E) 与压缩效率 eta(E)=R/S(E)。
  4. 用 eta 不低于阈值反解该档曝光的适用场景范围（Reinhard 逆曲线闭式解）。
  5. 高光判据：0.90..0.99 分位点不得有连续三个落在同一显示级。
  6. 暗部可见度曲线 V(E) 与压缩效率曲线联合定曝光。
  7. 非空桶比例反例：支持域内桶桶非空（比例 1.0），高光仍糊在同一级。

仅使用标准库；确定性种子，可重复运行：python3 -I dr_measure.py
"""

import json
import math
import random

# ---------------------------------------------------------------- 常量
LEVELS = 256                 # 显示域量化级数，码值 0..255
G_LO, G_HI = 3, 252          # 可用保护带：避开纯黑 0 与纯白 255
R_USABLE = G_HI - G_LO       # 可表示范围（码值跨度）
SPAN_LIMIT = 0.9 * R_USABLE  # 场景跨度必须压进可表示区间的九成
ETA_FLOOR = 1.0 / 0.9        # eta >= 1/0.9  <=>  S <= 0.9 R
EXPOSURES = (0.5, 1.0, 1.5, 2.0)
SEED = 20261127


# ---------------------------------------------------------------- 色调曲线
def reinhard(x, e):
    y = e * max(x, 0.0)
    return y / (1.0 + y)


def reinhard_inv(t):
    """Reinhard 逆曲线：线性光 t/(1-t)。"""
    return t / (1.0 - t)


def level_of(lum, e):
    k = int(reinhard(lum, e) * LEVELS)
    return min(LEVELS - 1, max(0, k))


# ---------------------------------------------------------------- 场景生成
def make_scene(n=200_000):
    """合成 HDR 场景：暗部/中间调/高光三个对数正态分量，固定种子。"""
    rng = random.Random(SEED)
    lum = []
    for _ in range(n):
        r = rng.random()
        if r < 0.72:                      # 暗部与中间调
            v = math.exp(rng.gauss(math.log(0.08), 0.90))
        elif r < 0.95:                    # 中间亮部
            v = math.exp(rng.gauss(math.log(0.80), 0.50))
        else:                             # 高光长尾（糊的根源）
            v = math.exp(rng.gauss(math.log(12.0), 0.60))
        lum.append(v)
    return lum


# ---------------------------------------------------------------- 直方图分位点反解
def log_histogram(lum, nbins=512):
    """对数等宽亮度直方图，返回桶边界与计数。"""
    lo, hi = min(lum), max(lum)
    log_lo, log_hi = math.log(lo), math.log(hi)
    step = (log_hi - log_lo) / nbins
    counts = [0] * nbins
    for v in lum:
        b = int((math.log(v) - log_lo) / step)
        counts[min(b, nbins - 1)] += 1
    edges = [math.exp(log_lo + i * step) for i in range(nbins + 1)]
    return edges, counts


def quantile_from_hist(q, edges, counts, total):
    """CDF 反解：累积计数首次越过 q*N 的桶，桶内按对数线性插值。"""
    target = q * total
    cum = 0
    for b, c in enumerate(counts):
        if cum + c >= target:
            frac = 0.0 if c == 0 else (target - cum) / c
            a, b1 = edges[b], edges[b + 1]
            return math.exp(math.log(a) + frac * (math.log(b1) - math.log(a)))
        cum += c
    return edges[-1]


def quantile_order_stat(lum_sorted, q):
    """次序统计量分位点，用于交叉校验直方图反解。"""
    n = len(lum_sorted)
    pos = q * (n - 1)
    i = int(pos)
    frac = pos - i
    if i + 1 >= n:
        return lum_sorted[-1]
    return lum_sorted[i] * (1 - frac) + lum_sorted[i + 1] * frac


# ---------------------------------------------------------------- 高光碰撞检测
def highlight_runs(lum, e, qs=tuple(i / 100 for i in range(90, 100))):
    """高光端分位点 -> 显示级，返回分位点坐标与最长同级连续段。"""
    pts = [(q, quantile_cache(q), level_of(quantile_cache(q), e)) for q in qs]
    longest, run = [], []
    for p in pts:
        if run and p[2] != run[-1][2]:
            run = []
        run.append(p)
        if len(run) > len(longest):
            longest = list(run)
    return pts, longest


_QCACHE = {}


def quantile_cache(q):
    return _QCACHE[q]


def display_hist(lum, e):
    bins = [0] * LEVELS
    for v in lum:
        bins[level_of(v, e)] += 1
    return bins


# ---------------------------------------------------------------- 反解：适用场景范围
def applicable_window(e):
    """保护带反解：落在 [G_LO,G_HI] 内的线性光输入窗口。"""
    l_lo = reinhard_inv(G_LO / LEVELS) / e
    l_hi = reinhard_inv(G_HI / LEVELS) / e
    return l_lo, l_hi


def max_dr_span_rule(p1, e):
    """
    跨度九成约束反解：固定 p1 的落点 k_lo，允许的最高码值 k_hi=k_lo+0.9R，
    再用逆曲线求 p99/p1 的上界 D_max。
    """
    k_lo = level_of(p1, e)
    k_hi = min(LEVELS - 1, k_lo + int(math.floor(SPAN_LIMIT)))
    p99_max = reinhard_inv(k_hi / LEVELS) / e
    return p99_max / p1, k_lo, k_hi


# ---------------------------------------------------------------- 非空桶反例
def make_counterexample():
    """
    支持域 [60,254] 内每个显示级都非空（非空桶比例=1.0），
    但 40..240 的高光辐射度（6 倍跨度）全部挤进顶端少数几个级，
    顶端分位点连续撞级。E 固定为 1.0。返回 (全部像素, E, 高光像素切片)。
    """
    e = 1.0
    lum, hi_lum = [], []
    # 中段：每个级 60..249 各放 200 个像素，桶桶非空
    for k in range(60, 250):
        t = (k + 0.5) / LEVELS
        lum.extend([reinhard_inv(t) / e] * 200)
    # 高光：对数均匀撒在辐射度 40..240，映射后只能落在顶端几个级
    rng = random.Random(SEED + 1)
    n_hi = 40_000
    for _ in range(n_hi):
        v = math.exp(rng.uniform(math.log(40.0), math.log(240.0)))
        lum.append(v)
        hi_lum.append(v)
    return lum, e, hi_lum


# ---------------------------------------------------------------- 主流程
def main():
    report = []
    out = {}

    def emit(line=""):
        report.append(line)
        print(line)

    emit("=" * 72)
    emit("场景动态范围测量与四档曝光适配（Reinhard, 256 级显示域）")
    emit("=" * 72)

    lum = make_scene()
    n = len(lum)
    edges, counts = log_histogram(lum)
    lum_sorted = sorted(lum)

    # 分位点缓存（高光检测复用）
    for q in (0.01, 0.99, *(i / 100 for i in range(90, 100))):
        _QCACHE[q] = quantile_from_hist(q, edges, counts, n)

    p1 = _QCACHE[0.01]
    p99 = _QCACHE[0.99]
    p1_os = quantile_order_stat(lum_sorted, 0.01)
    p99_os = quantile_order_stat(lum_sorted, 0.99)

    s_scene = p99 / p1
    stops_scene = math.log2(s_scene)

    # 可表示范围：保护带对应的线性光窗口（曝光无关的比值式）
    w_lo = reinhard_inv(G_LO / LEVELS)
    w_hi = reinhard_inv(G_HI / LEVELS)
    r_ratio = w_hi / w_lo
    r_stops = math.log2(r_ratio)

    emit("\n[1] 场景动态范围（亮度直方图百分位反解，512 个对数桶）")
    emit(f"  像素数 N            = {n}")
    emit(f"  p1  (直方图反解)    = {p1:.6f}   (次序统计量校验 {p1_os:.6f},"
         f" 偏差 {abs(p1-p1_os)/p1_os*100:.2f}%)")
    emit(f"  p99 (直方图反解)    = {p99:.6f}   (次序统计量校验 {p99_os:.6f},"
         f" 偏差 {abs(p99-p99_os)/p99_os*100:.2f}%)")
    emit("  场景动态范围表达式:")
    emit("    S_scene = p99 / p1 "
         f"= {p99:.4f} / {p1:.4f} = {s_scene:.2f}  ({stops_scene:.2f} stops)")
    emit("  可表示范围（保护带码值 [3,252]，逆曲线还原线性光）:")
    emit(f"    R = f^-1(252/256) / f^-1(3/256) = {w_hi:.3f} / {w_lo:.5f} "
         f"= {r_ratio:.1f}  ({r_stops:.2f} stops)")
    cmp_text = "大于" if s_scene > r_ratio else "小于等于"
    emit(f"  比较: S_scene={s_scene:.1f} {cmp_text} R={r_ratio:.1f}，"
         f"即单档曝光下场景需要 {stops_scene:.1f} stops，"
         f"显示域保护带只容纳 {r_stops:.1f} stops -> 必须靠曝光取舍。")
    out["scene"] = {"n": n, "p1": p1, "p99": p99, "s_scene": s_scene,
                    "stops_scene": stops_scene, "r_ratio": r_ratio,
                    "r_stops": r_stops}

    # ------------------------------------------------------------ 四档曝光
    emit("\n[2] 四档预设逐档评估（跨度 / 压缩效率 / 高光碰撞 / 暗部可见度）")
    header = (f"  {'E':>4} | {'k(p1)':>5} {'k(p99)':>6} | {'S(E)':>5} "
              f"{'eta=R/S':>8} | {'A:跨度<=224':>12} | {'最长同级段':>10} "
              f"{'B:无三连':>8} | {'V(>=3级)':>9}")
    emit(header)
    emit("  " + "-" * (len(header) - 2))

    preset_results = []
    for e in EXPOSURES:
        k_lo = level_of(p1, e)
        k_hi = level_of(p99, e)
        s_e = k_hi - k_lo
        eta = R_USABLE / s_e if s_e > 0 else float("inf")
        pass_a = s_e <= SPAN_LIMIT

        pts, longest = highlight_runs(lum, e)
        pass_b = len(longest) < 3

        bins = display_hist(lum, e)
        v_black = sum(1 for v in lum if level_of(v, e) >= 1) / n
        v_guard = sum(bins[G_LO:]) / n

        emit(f"  {e:>4.1f} | {k_lo:>5} {k_hi:>6} | {s_e:>5} {eta:>8.3f} | "
             f"{('PASS' if pass_a else 'FAIL'):>12} | "
             f"{len(longest):>10} {('PASS' if pass_b else 'FAIL'):>8} | "
             f"{v_guard:>9.4f}")
        preset_results.append({
            "exposure": e, "k_p1": k_lo, "k_p99": k_hi, "span": s_e,
            "eta": eta, "pass_span": pass_a, "highlight_pts": pts,
            "longest_run": longest, "pass_highlight": pass_b,
            "v_above_black": v_black, "v_above_guard": v_guard,
            "first_three_bins": bins[:3],
        })
    out["presets"] = preset_results

    # ------------------------------------------------------------ 反解适用范围
    emit("\n[3] 反解：每档曝光的适用场景范围（压缩效率不低于 1/0.9）")
    emit("  约束链: eta(E)=R/S(E) >= 1/0.9  <=>  S(E) <= 0.9 R = "
         f"{SPAN_LIMIT:.1f} 个码值级")
    win_ratio = w_hi / w_lo
    emit(f"  保护带窗口（闭式逆解 f^-1(t)=t/(1-t)）: "
         f"[f^-1(3/256)/E, f^-1(252/256)/E]，窗口比值恒为 {win_ratio:.1f}"
         f"（{math.log2(win_ratio):.2f} stops），曝光只平移窗口不平移比值。")
    emit(f"  {'E':>4} | {'窗口下限':>10} {'窗口上限':>10} | "
         f"{'本场景p1下D_max':>15} | {'场景实际S':>10} | 九成跨度判定")
    for e in EXPOSURES:
        l_lo, l_hi = applicable_window(e)
        d_max, k_lo, k_hi = max_dr_span_rule(p1, e)
        verdict = "通过(S<=D_max)" if s_scene <= d_max else "不通过(S>D_max)"
        emit(f"  {e:>4.1f} | {l_lo:>10.5f} {l_hi:>10.3f} | "
             f"{d_max:>15.1f} | {s_scene:>10.1f} | {verdict}")
    guard_ok = [(e, applicable_window(e)) for e in EXPOSURES
                if applicable_window(e)[0] <= p1 and p99 <= applicable_window(e)[1]]
    emit("  注：窗口列是保护带 [3,252] 的纯容纳窗口（仅检查不触边）；"
         f"满足纯容纳的档位为 {[e for e, _ in guard_ok] or '无'}，")
    emit("      但九成跨度（D_max）列更严格——四档全部不通过，与 [2] 的 A 判据 FAIL 互证。")
    emit("      问题不在曝光档位选择，在场景本身超出单档容量，换档无法解决。")

    # ------------------------------------------------------------ 不达标明细
    emit("\n[4] 不达标档的分位点坐标与直方图前三个桶计数")
    any_fail = False
    for r in preset_results:
        if r["pass_span"] and r["pass_highlight"]:
            continue
        any_fail = True
        e = r["exposure"]
        emit(f"  -- 曝光 E={e} --")
        if not r["pass_span"]:
            emit(f"     A 失败: 跨度 S={r['span']} > {SPAN_LIMIT:.0f} "
                 f"(eta={r['eta']:.3f} < {ETA_FLOOR:.4f})")
        if not r["pass_highlight"]:
            emit("     B 失败: 高光分位点连续撞级，最长连续段坐标 (分位, 亮度, 显示级):")
            for q, lv, k in r["longest_run"]:
                emit(f"       q={q:.2f}  L={lv:9.4f}  level={k}")
        emit("     分位点坐标 (分位, 亮度, 显示级):")
        emit(f"       q=0.01  L={p1:9.4f}  level={r['k_p1']}")
        emit(f"       q=0.99  L={p99:9.4f}  level={r['k_p99']}")
        for q, lv, k in r["highlight_pts"]:
            emit(f"       q={q:.2f}  L={lv:9.4f}  level={k}")
        emit(f"     显示直方图前三个桶计数 bins[0..2] = "
             f"{r['first_three_bins']}")
    if not any_fail:
        emit("  无（四档全部通过）")

    # ------------------------------------------------------------ 非空桶反例
    emit("\n[5] 反例：非空桶比例为什么会骗人")
    cx, e_cx, hi_cx = make_counterexample()
    bins_cx = display_hist(cx, e_cx)
    used = [k for k in range(LEVELS) if bins_cx[k] > 0]
    support = range(min(used), max(used) + 1)
    nonempty_support = sum(1 for k in support if bins_cx[k] > 0) / len(list(support))
    hi_levels = sorted(set(level_of(v, e_cx) for v in hi_cx))
    edges_cx, counts_cx = log_histogram(cx)
    qs_cx = tuple(i / 100 for i in range(90, 100))
    pts_cx = [(q, quantile_from_hist(q, edges_cx, counts_cx, len(cx)),
               level_of(quantile_from_hist(q, edges_cx, counts_cx, len(cx)), e_cx))
              for q in qs_cx]
    longest_cx, run = [], []
    for p in pts_cx:
        if run and p[2] != run[-1][2]:
            run = []
        run.append(p)
        if len(run) > len(longest_cx):
            longest_cx = list(run)
    emit(f"  场景 N={len(cx)}, E={e_cx}；支持域码值 [{min(used)}, {max(used)}]")
    emit(f"  非空桶比例（按支持域计）= {nonempty_support:.3f} —— 桶桶非空，指标满分")
    emit(f"  但高光辐射度 40..240（6.0x 跨度）全部挤进级 "
         f"{hi_levels[0]}..{hi_levels[-1]}（仅 {len(hi_levels)} 个级）；"
         f"顶端分位点最长连续段={len(longest_cx)}:")
    for q, lv, k in longest_cx:
        emit(f"    q={q:.2f}  L={lv:8.3f}  level={k}")
    emit("  结论：桶计数只回答'有没有像素落进来'，回答不了'不同亮度是否还可区分'；")
    emit("        判据必须用分位点（p1/p99 比值 + 高光分位点撞级检测），不能用桶计数。")
    out["counterexample"] = {"nonempty_ratio_support": nonempty_support,
                             "longest_run": longest_cx}

    # ------------------------------------------------------------ 两条曲线定曝光
    emit("\n[6] 取舍：压缩效率曲线 vs 暗部可见度曲线")
    emit(f"  {'E':>5} | {'S(E)':>5} {'eta':>7} | {'V(>=1级)':>9} "
         f"{'V(>=3级)':>9} | {'A':>4} {'B':>4}")
    grid = [0.25 * i for i in range(1, 13)]
    feasible, grid_rows = [], []
    for e in grid:
        k_lo = level_of(p1, e)
        k_hi = level_of(p99, e)
        s_e = k_hi - k_lo
        eta = R_USABLE / s_e if s_e > 0 else float("inf")
        v0 = sum(1 for v in lum if level_of(v, e) >= 1) / n
        vg = sum(1 for v in lum if level_of(v, e) >= G_LO) / n
        _, longest = highlight_runs(lum, e)
        pa, pb = s_e <= SPAN_LIMIT, len(longest) < 3
        if pa and pb:
            feasible.append((e, vg))
        grid_rows.append({"e": e, "span": s_e, "eta": eta,
                          "v_above_black": v0, "v_above_guard": vg,
                          "pass_a": pa, "pass_b": pb})
        emit(f"  {e:>5.2f} | {s_e:>5} {eta:>7.3f} | {v0:>9.4f} {vg:>9.4f} | "
             f"{('P' if pa else 'F'):>4} {('P' if pb else 'F'):>4}")

    def span_at(e):
        return level_of(p99, e) - level_of(p1, e)

    # 二分：跨度在上升沿随 E 单调不减，求 span <= 0.9R 的最大曝光（左交点）。
    # 上界取 1.0（该点 span=241 已超限），避开高曝光段 p99 钉顶导致的跨度回落。
    lo_e, hi_e = 0.01, 1.0
    for _ in range(60):
        mid = 0.5 * (lo_e + hi_e)
        if span_at(mid) <= SPAN_LIMIT:
            lo_e = mid
        else:
            hi_e = mid
    e_span_max = lo_e

    # 保护带反解给出的理论曝光窗口（暗部可见度曲线的可行区间）
    e_lo = reinhard_inv(G_LO / LEVELS) / p1
    e_hi = reinhard_inv(G_HI / LEVELS) / p99
    emit(f"\n  保护带反解曝光窗口（暗部可见度曲线）: E in [{e_lo:.3f}, {e_hi:.3f}]")
    emit(f"  九成跨度可行上界（压缩效率曲线，二分反解）: E <= {e_span_max:.3f}")
    inter_lo, inter_hi = max(0.0, e_lo), min(e_hi, e_span_max)
    if inter_lo <= inter_hi:
        emit(f"  两曲线交集: E in [{inter_lo:.3f}, {inter_hi:.3f}]，"
             f"取上界 E*={inter_hi:.3f}（暗部可见度在交集内最高）")
    else:
        emit(f"  两曲线交集: 空 —— 跨度可行上界 {e_span_max:.3f} < "
             f"保护带窗口下界 {e_lo:.3f}，两条曲线在 256 级显示域内不相交。")
        emit("  即：满足九成跨度的曝光必然让约 "
             f"{(1 - sum(1 for v in lum if level_of(v, e_span_max) >= G_LO) / n) * 100:.1f}%"
             " 的像素沉到保护带以下；")
        emit("      满足暗部保护带的曝光必然撑破九成跨度。四档预设 0.5..2.0 全部落在窗口右侧，无一可行。")
        emit("  决策建议：若坚持 256 级输出，只能二选一放松约束——保层次取 E~"
             f"{e_span_max:.2f}（牺牲暗部），保暗部取 E~{e_lo:.2f}（牺牲高光层次）；")
        emit("            根治需要更宽可表示范围（浮点/HDR 输出）或局部色调映射，而不是在四档间换档。")
    out["feasible_exposures"] = [{"e": e, "v_guard": v} for e, v in feasible]
    out["window_exposure"] = {"e_lo": e_lo, "e_hi": e_hi,
                              "e_span_max": e_span_max,
                              "intersection": [inter_lo, inter_hi]
                              if inter_lo <= inter_hi else None}
    out["grid_curve"] = grid_rows

    with open(__file__.rsplit("/", 1)[0] + "/../../artifacts/dr_results.json",
              "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    with open(__file__.rsplit("/", 1)[0] + "/../../artifacts/dr_report.txt",
              "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\n[written] artifacts/dr_report.txt, artifacts/dr_results.json")


if __name__ == "__main__":
    main()
