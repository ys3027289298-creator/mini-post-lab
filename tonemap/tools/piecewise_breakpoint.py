#!/usr/bin/env python3
"""HDR->SDR 分段点求解与三档验证.

规范曲线 f: 暗段线性 + 亮段幂 (分段点 x_b, 指数 gamma 为规范常数).
我们的曲线:
  方案A  g0 = x^ghat            全局幂 (现状, ghat 按场景直方图拟合)
  方案A' g(x;t) = a(t)*x (x<=t), x^ghat (x>t)   分段, a(t)=t^(ghat-1) 保 C0 连续
  方案B  g(x;t) = a(t)*x (x<=t), x^gamma (x>t)  亮段锚定规范指数

判据: 场景加权平均绝对误差 E(t) <= DELTA = 1/255 (一个输出级).
等误差准则: 暗段相对误差 == 亮段相对误差 -> 解 t_eq.
只依赖标准库, 直接 `python3 tonemap/tools/piecewise_breakpoint.py` 复现.
"""
import math

GAMMA = 1.0 / 2.2          # 规范亮段幂指数
XB = 0.10                  # 规范分段点 (归一化 HDR 输入)
K = XB ** (GAMMA - 1.0)    # 规范暗段斜率, C0 连续
DELTA = 1.0 / 255.0        # 一个输出级 (8bit)

N = 20000
XS = [(i + 0.5) / N for i in range(N)]


def f_ref(x):
    return K * x if x <= XB else x ** GAMMA


FS = [f_ref(x) for x in XS]


def scene_weights(mu, s=0.9):
    """对数正态亮度直方图, 均值恰为 mu."""
    m = math.log(mu) - s * s / 2.0
    w = [math.exp(-((math.log(x) - m) ** 2) / (2 * s * s)) / x for x in XS]
    z = sum(w)
    return [wi / z for wi in w]


def fit_ghat(w):
    """全局幂指数: 场景加权最小二乘."""
    def mse(gh):
        return sum(wi * (x ** gh - fi) ** 2 for wi, x, fi in zip(w, XS, FS))
    return _golden(mse, 0.25, 1.0)


def _golden(func, a, b, it=90):
    gr = (math.sqrt(5.0) - 1.0) / 2.0
    c, d = b - gr * (b - a), a + gr * (b - a)
    for _ in range(it):
        if func(c) < func(d):
            b, d = d, c
            c = b - gr * (b - a)
        else:
            a, c = c, d
            d = a + gr * (b - a)
    return (a + b) / 2.0


def seg_errors(w, gh, t):
    """按参考分段 [0,x_b],[x_b,1] 累计场景加权绝对误差; 返回 (Ed,Eb,Fd,Fb)."""
    a = t ** (gh - 1.0)
    ed = eb = fd = fb = 0.0
    for wi, x, fi in zip(w, XS, FS):
        g = a * x if x <= t else x ** gh
        e = wi * abs(g - fi)
        if x <= XB:
            ed += e
            fd += wi * fi
        else:
            eb += e
            fb += wi * fi
    return ed, eb, fd, fb


def overall_E(w, gh, t):
    ed, eb, _, _ = seg_errors(w, gh, t)
    return ed + eb


def solve_equal_error(w, gh):
    """等误差准则: Ed/Fd == Eb/Fb, 二分求解."""
    def h(t):
        ed, eb, fd, fb = seg_errors(w, gh, t)
        return ed / fd - eb / fb
    lo, hi = 1e-3, 0.95
    if h(lo) * h(hi) > 0:
        return min((i / 1000.0 for i in range(1, 950)), key=lambda t: abs(h(t)))
    for _ in range(60):
        mid = (lo + hi) / 2.0
        if h(lo) * h(mid) <= 0:
            hi = mid
        else:
            lo = mid
    return (lo + hi) / 2.0


def allowed_range(w, gh):
    """{t : E(t) <= DELTA}; 空集返回 None."""
    t_opt = _golden(lambda t: overall_E(w, gh, t), 0.02, 0.6)
    e_opt = overall_E(w, gh, t_opt)
    if e_opt > DELTA:
        return t_opt, e_opt, None
    lo, hi = 0.02, t_opt
    for _ in range(60):
        m = (lo + hi) / 2.0
        if overall_E(w, gh, m) > DELTA:
            lo = m
        else:
            hi = m
    t_lo = hi
    lo, hi = t_opt, 0.6
    for _ in range(60):
        m = (lo + hi) / 2.0
        if overall_E(w, gh, m) > DELTA:
            hi = m
        else:
            lo = m
    return t_opt, e_opt, (t_lo, lo)


def E_global(w, gh):
    return sum(wi * abs(x ** gh - fi) for wi, x, fi in zip(w, XS, FS))


def dark_rel_global(w, gh):
    num = den = 0.0
    for wi, x, fi in zip(w, XS, FS):
        if x <= XB:
            num += wi * abs(x ** gh - fi)
            den += wi * fi
    return num / den


LEVELS = [("暗", 0.05), ("中", 0.18), ("亮", 0.50)]

print("=" * 72)
print(f"规范曲线: f = {K:.4f}*x (x<={XB}), x^{GAMMA:.4f} (x>{XB});  预算 DELTA = 1/255 = {DELTA:.5f}")
print("=" * 72)

# ---------- 反例: 分段点取在暗部之外 (中档) ----------
w_mid = scene_weights(0.18)
gh_mid = fit_ghat(w_mid)
eg_mid = E_global(w_mid, gh_mid)
print("\n[反例] 中档场景, 分段点 t_bad 取在暗部之外 (x_b=0.10):")
print(f"  {'t_bad':>6} {'E_dark':>9} {'E_bright':>9} {'E_total':>9}  对比全局幂 E_g0={eg_mid:.5f}")
for tb in (0.20, 0.30, 0.50):
    ed, eb, _, _ = seg_errors(w_mid, gh_mid, tb)
    print(f"  {tb:6.2f} {ed:9.5f} {eb:9.5f} {ed+eb:9.5f}  -> 是全局幂的 {(ed+eb)/eg_mid:.1f} 倍")

# ---------- 方案B 允许偏移范围 (亮段锚定规范 gamma) ----------
print("\n[方案B] 亮段锚定规范指数 gamma, 由 E(t)<=1/255 反解允许偏移范围:")
rangeB = {}
for name, mu in LEVELS:
    w = scene_weights(mu)
    t_opt, e_opt, rng = allowed_range(w, GAMMA)
    rangeB[name] = rng
    if rng:
        print(f"  {name}档(mu={mu}): t_opt={t_opt:.4f} E={e_opt:.6f} "
              f"允许范围 [{rng[0]:.4f},{rng[1]:.4f}] 偏移 [{rng[0]-XB:+.4f},{rng[1]-XB:+.4f}]")
# 线性近似对照 (中档): E 在 x_b 附近呈 V 形 (斜率失配 |a(t)-k| 线性进入误差)
h = 1e-3
slope = overall_E(w_mid, GAMMA, XB + h) / h  # E(x_b)=0
print(f"  (对照) 中档 |E'(x_b)|~{slope:.3f}, 线性近似 delta=DELTA/|E'|={DELTA/slope:.4f} (精确见上)")

# ---------- 三档实测 (方案A': 分段, 亮段用拟合指数 ghat) ----------
print("\n[三档实测] 方案A': 分段线性+拟合幂, 等误差准则解 t*:")
print(f"  {'档':>3} {'mu':>5} {'ghat':>7} {'E_g0':>8} {'暗部相对误差':>10} "
      f"{'t*':>7} {'t*-x_b':>8} {'E(t*)':>8} {'t_opt':>7} {'E(t_opt)':>9} {'判定':>6}")
results = []
for name, mu in LEVELS:
    w = scene_weights(mu)
    gh = fit_ghat(w)
    eg = E_global(w, gh)
    drel = dark_rel_global(w, gh)
    t_eq = solve_equal_error(w, gh)
    e_eq = overall_E(w, gh, t_eq)
    t_opt, e_opt, rng = allowed_range(w, gh)
    ok_e = e_eq <= DELTA
    rng_b = rangeB[name]
    ok_off = rng_b is not None and rng_b[0] <= t_eq <= rng_b[1]
    verdict = "达标" if (ok_e and ok_off) else "超标"
    results.append((name, mu, gh, eg, drel, t_eq, e_eq, t_opt, e_opt, rng))
    print(f"  {name:>3} {mu:5.2f} {gh:7.4f} {eg:8.5f} {drel:10.3f} "
          f"{t_eq:7.4f} {t_eq-XB:+8.4f} {e_eq:8.5f} {t_opt:7.4f} {e_opt:9.5f} {verdict:>6}")

print("\n[超标明细] 实测误差与两个候选分段点 (允许范围为空时, 候选取等误差点 t_eq 与总误差最小点 t_opt):")
for name, mu, gh, eg, drel, t_eq, e_eq, t_opt, e_opt, rng in results:
    if e_eq > DELTA:
        print(f"  {name}档(mu={mu}): 实测 E(t*)={e_eq:.5f} (预算 {DELTA:.5f}, 超 {e_eq/DELTA:.1f} 倍); "
              f"允许范围={'空集' if rng is None else rng}; "
              f"候选1 t_eq={t_eq:.4f} (E={e_eq:.5f}), 候选2 t_opt={t_opt:.4f} (E={e_opt:.5f})")

# ---------- 亮段指数容差 ----------
print("\n[亮段指数容差] |ghat-gamma| <= DELTA / S, S = 亮段积分 w*x^gamma*|ln x|:")
for name, mu in LEVELS:
    w = scene_weights(mu)
    gh = fit_ghat(w)
    s = sum(wi * (x ** GAMMA) * abs(math.log(x)) for wi, x in zip(w, XS) if x > XB)
    print(f"  {name}档: |ghat-gamma|={abs(gh-GAMMA):.4f}, 容差={DELTA/s:.4f} -> "
          f"{'满足' if abs(gh-GAMMA) <= DELTA/s else '超出 %.1f 倍' % (abs(gh-GAMMA)/(DELTA/s))}")

# ---------- 取舍 ----------
egs = "/".join(f"{r[3]:.4f}" for r in results)
eas = "/".join(f"{r[6]:.4f}" for r in results)
print("\n[取舍] 整体误差 vs 逐像素开销 (开销归一: 1 次 pow = 20 cycle, 比较+选择 ~2 cycle, 乘 ~1 cycle, LUT ~2 cycle):")
print(f"  {'方案':<22} {'整体误差(暗/中/亮)':<26} {'逐像素开销':<18}")
print(f"  {'全局幂 (现状)':<22} {egs:<26} {'20 cycle':<18}")
print(f"  {'分段A (亮段拟合幂)':<22} {eas:<26} {'暗3/亮22 cycle':<18}")
print(f"  {'分段B (亮段规范幂)':<22} {'<=0.0039 (t在允许范围内)':<26} {'暗3/亮22 cycle':<18}")
print(f"  {'分段B + 每档LUT':<22} {'同上':<26} {'2 cycle (与全局幂LUT相同)':<18}")
print("  结论: 误差的瓶颈是亮段指数而非分段点; 采用方案B, t 取允许范围中点, LUT 化后逐像素开销与现状持平.")
