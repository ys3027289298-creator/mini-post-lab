#!/usr/bin/env python3
"""
决策模型：T = P·c_fix + N·B·t_b（计算量三模式相同，差值中相消）。

  sep   P=3, B_sep   = 22(读) + 14(原生写) = 36 B/px
  mrt   P=1, B_mrt   = 22(读) + 3s(统一格式写)
  split P=2, B_split = 22(读) + 12(写: RGBA8+RGBA16F) = 34 B/px

从实测 min 耗时最小二乘拟合 t_b（c_fix 取自旋屏障校准值），
再用「T_mrt ≤ T_sep」反解每档分辨率可接受的统一格式，输出判定表。
"""
import json
import re
import statistics
import sys
from collections import defaultdict

C_FIX_S = 6.5325e-6          # 自旋屏障校准：每次派发秒数
NATIVE_WRITE = {"color": 4, "coc": 2, "bloom": 8}   # 原生格式 B/px
COMMON_FORMATS = {"RGBA8": 4, "RGBA16F": 8, "RGBA32F": 16}
READ_B = 22
B_SEP = READ_B + sum(NATIVE_WRITE.values())        # 36
B_SPLIT = READ_B + 4 + 8                           # 34

PAT = re.compile(
    r"mode=(\w+) res=(\d+)x(\d+).*?passes=(\d+).*?per_frame_ms=([\d.]+)"
)


def load(path):
    rows = defaultdict(list)
    for line in open(path):
        m = PAT.search(line)
        if m:
            mode, w, h, passes, ms = m.groups()
            rows[(int(w), int(h), mode)].append(float(ms) / 1000.0)
    return {k: min(v) for k, v in rows.items()}   # min-of-N：未被抢占的最快一轮


def fit(mins):
    """T = P·c_fix + N·B·t_b + N·t_alu：对 t_b、t_alu 做二元最小二乘。

    t_alu 是三模式相同的每像素计算量（ACES+CoC+bright-pass 每帧都跑一遍），
    在模式差值中相消，但必须建模才能让绝对值拟合成立。
    """
    a1, a2 = [], []   # 列：N·B（带宽）、N（计算）
    y = []
    for (w, h, mode), t in mins.items():
        n = w * h
        if mode == "mrt":
            p, b = 1, READ_B + 3 * COMMON_FORMATS["RGBA16F"]
        elif mode == "sep":
            p, b = 3, B_SEP
        else:
            p, b = 2, B_SPLIT
        a1.append(n * b)
        a2.append(n)
        y.append(t - p * C_FIX_S)
    # 正规方程（2x2）
    s11 = sum(v * v for v in a1)
    s22 = sum(v * v for v in a2)
    s12 = sum(u * v for u, v in zip(a1, a2))
    r1 = sum(u * v for u, v in zip(a1, y))
    r2 = sum(u * v for u, v in zip(a2, y))
    det = s11 * s22 - s12 * s12
    t_b = (r1 * s22 - r2 * s12) / det
    t_alu = (s11 * r2 - s12 * r1) / det
    return t_b, t_alu


def main():
    mins = load(sys.argv[1])
    t_b, t_alu = fit(mins)
    print(f"拟合 t_b = {t_b*1e9:.4f} ns/(B·px)，t_alu = {t_alu*1e9:.4f} ns/px，"
          f"c_fix = {C_FIX_S*1e6:.2f} us/次\n")

    resolutions = sorted({(w, h) for (w, h, _) in mins})
    report = {"t_b_s": t_b, "t_alu_s": t_alu, "c_fix_s": C_FIX_S, "resolutions": []}

    def predict(w, h, mode):
        n = w * h
        if mode == "mrt":
            p, b = 1, READ_B + 3 * COMMON_FORMATS["RGBA16F"]
        elif mode == "sep":
            p, b = 3, B_SEP
        else:
            p, b = 2, B_SPLIT
        return p * C_FIX_S + n * b * t_b + n * t_alu

    print(f"{'分辨率':>10} {'模式':>6} {'实测ms':>8} {'模型ms':>8} {'误差':>7}")
    for w, h in resolutions:
        for mode in ("sep", "mrt", "split"):
            meas, pred = mins[(w, h, mode)] * 1e3, predict(w, h, mode) * 1e3
            print(f"{w}x{h:>5} {mode:>6} {meas:>8.3f} {pred:>8.3f} "
                  f"{(pred/meas-1)*100:>+6.1f}%")
    print()

    print(f"{'分辨率':>10} {'N(px)':>9} {'sep实测':>9} {'mrt实测':>9} "
          f"{'split实测':>10} {'mrt回归':>8} {'split回归':>9}")
    for w, h in resolutions:
        n = w * h
        t_sep, t_mrt, t_split = (mins[(w, h, m)] for m in ("sep", "mrt", "split"))
        print(f"{w}x{h:>5} {n:>9} {t_sep*1e3:>8.3f}m {t_mrt*1e3:>8.3f}m "
              f"{t_split*1e3:>9.3f}m {(t_mrt/t_sep-1)*100:>+7.1f}% "
              f"{(t_split/t_sep-1)*100:>+8.1f}%")

    print("\n== 反解：统一格式 s 满足 T_mrt(s) ≤ T_sep ==")
    print("条件: 3s - 14 ≤ 2·c_fix / (N·t_b)   （t_alu 两侧相等，自动消去）\n")
    for w, h in resolutions:
        n = w * h
        budget = 2 * C_FIX_S / (n * t_b)          # 允许的额外写带宽 B/px
        s_max = (sum(NATIVE_WRITE.values()) + budget) / 3
        print(f"{w}x{h}: 预算 Δwrite ≤ {budget:6.2f} B/px → s ≤ {s_max:5.2f} B/px")
        for name, s in COMMON_FORMATS.items():
            delta_write = 3 * s - sum(NATIVE_WRITE.values())
            ok = delta_write <= budget
            t_mrt_s = C_FIX_S + n * (READ_B + 3 * s) * t_b + n * t_alu
            t_sep = predict(w, h, "sep")
            note = ""
            if name == "RGBA8":
                note = "（内容非法：HDR bloom 无法装入 LDR8）"
            print(f"   {'✓' if ok and not note else '✗'} {name:7s} s={s:2d}B "
                  f"Δwrite={delta_write:+3d}B  T_mrt={t_mrt_s*1e3:7.3f}ms "
                  f"vs T_sep={t_sep*1e3:7.3f}ms {note}")
        print()

    print("== 被迫升级的目标（MRT 统一到 RGBA16F 相对原生格式）==")
    n_max = max(w * h for w, h in resolutions)
    for tgt, b in NATIVE_WRITE.items():
        d = COMMON_FORMATS["RGBA16F"] - b
        tag = "被迫升级" if d > 0 else "无需升级（原生即 RGBA16F）"
        print(f"   {tgt:6s}: {b}B → 8B，每像素 +{d}B，1440p 整帧 +{d*n_max/1e6:.2f} MB"
              f"  [{tag}]")

    # 图表数据：两条曲线随 N 变化（t_alu 三模式相同，曲线仍有意义）
    curve = {"n": [], "sep": [], "mrt": [], "split": []}
    for w, h in resolutions:
        n = w * h
        curve["n"].append(n)
        curve["sep"].append(predict(w, h, "sep") * 1e3)
        curve["mrt"].append(predict(w, h, "mrt") * 1e3)
        curve["split"].append(predict(w, h, "split") * 1e3)
    report["curve_ms"] = curve
    report["measured_ms"] = {
        f"{w}x{h}": {m: mins[(w, h, m)] * 1e3 for m in ("sep", "mrt", "split")}
        for w, h in resolutions
    }
    json.dump(report, open("model_fit.json", "w"), indent=2)
    print("\n已写出 model_fit.json")


if __name__ == "__main__":
    main()
