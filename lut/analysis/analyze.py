#!/usr/bin/env python3
"""
analyze.py — verify the analytical LUT error model against the C benchmark
data and generate the trade-off report (self-contained HTML/SVG).

Stdlib-only so it runs under `python3 -I`; reads only analysis/sweep.csv.
"""
import bisect
import csv
import html
import json
import math
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
N = 17
JND = 2.3
K = 0.18
X_DARK, X_MID, X_BRIGHT = 0.05, 0.18, 0.50
HALF_W = 0.02

# ---------------------------------------------------------------- model
def f(x):
    """baked curve, mirrors lut_curve() in src/lut.c"""
    x = max(x, 0.0)
    return min(x * (1.0 + K) / (x + K), 1.0)

def f_pp(x):
    """second derivative, closed form: -2(1+k)k/(x+k)^3"""
    return -2.0 * (1.0 + K) * K / (x + K) ** 3

def lab_l(y):
    y = max(y, 0.0)
    return 116.0 * y ** (1.0 / 3.0) - 16.0 if y > 0.008856 else 903.3 * y

def nodes(p, n=N):
    return [(i / (n - 1)) ** p for i in range(n)]

def linspace(lo, hi, count):
    if count == 1:
        return [lo]
    return [lo + (hi - lo) * i / (count - 1) for i in range(count)]

def lut_sample_1d(x, xs, ys):
    """trilinear on a separable gray bake collapses to this 1-D blend;
    the C side still executes the full 8-corner 3-D weighting."""
    c = bisect.bisect_right(xs, x) - 1
    c = max(0, min(c, len(xs) - 2))
    t = (x - xs[c]) / (xs[c + 1] - xs[c])
    return ys[c] * (1.0 - t) + ys[c + 1] * t

def tier_dl(p, center, half=HALF_W):
    xs = nodes(p)
    ys = [f(v) for v in xs]
    lo = max(0.0, center - half)
    hi = min(1.0, center + half)
    worst = 0.0
    for x in linspace(lo, hi, 401):
        d = abs(lab_l(lut_sample_1d(x, xs, ys)) - lab_l(f(x)))
        worst = max(worst, d)
    return worst

# ------------------------------------------------- 1. trilinear bound
def check_trilinear_bound():
    xs, ys = nodes(1.0), [f(v) for v in nodes(1.0)]
    worst = 0.0
    for c in range(N - 1):
        dense = linspace(xs[c], xs[c + 1], 401)
        errs = [abs(lut_sample_1d(x, xs, ys) - f(x)) for x in dense]
        emax = max(errs)
        delta = abs(ys[c + 1] - ys[c])
        bound = 0.25 * delta
        assert emax <= bound + 1e-12, (c, emax, bound)
        # The leading term peaks at t = 1/2 (cell center). The f''' remainder
        # shifts the observed argmax toward the higher-curvature (lower) end;
        # check that shift stays an O(h) fraction of the cell, never beyond
        # t in [0.4, 0.6].
        x_at = dense[errs.index(emax)]
        t_at = (x_at - xs[c]) / (xs[c + 1] - xs[c])
        assert 0.4 <= t_at <= 0.6, (c, t_at)
        worst = max(worst, emax / bound)
    return worst

# --------------------------------------- 2. dark/bright perceptual ratio
def perceptual_ratio_expression(xd, xb):
    """closed form: (xb/xd)^(2/3) * ((xb+k)/(xd+k))^3"""
    return (xb / xd) ** (2.0 / 3.0) * ((xb + K) / (xd + K)) ** 3

# ------------------------------------------ 3. analytical spacing design
def required_spacing(x, jnd=JND):
    """h(x) <= sqrt(JND / [(116/24) x^-2/3 |f''(x)|])"""
    coeff = (116.0 / 24.0) * x ** (-2.0 / 3.0) * abs(f_pp(x))
    return math.sqrt(jnd / coeff)

def local_spacing(x, p):
    """power-law local spacing: h(x) = p x^(1-1/p)/(N-1)"""
    return p * x ** (1.0 - 1.0 / p) / (N - 1)

def main():
    report = {}

    ratio_bound = check_trilinear_bound()
    report["peak_over_quarter_bound"] = ratio_bound
    print(f"[1] trilinear Delta/4 bound holds on every cell; "
          f"worst measured/bound = {ratio_bound:.4f}")

    r_expr = perceptual_ratio_expression(X_DARK, X_BRIGHT)
    dl_dark = tier_dl(1.0, X_DARK)
    dl_mid_u = tier_dl(1.0, X_MID)
    dl_bright = tier_dl(1.0, X_BRIGHT)
    report["ratio_expr"] = r_expr
    report["ratio_measured"] = dl_dark / dl_bright
    print(f"[2] shadow/highlight perceptual ratio: expression={r_expr:.1f}x  "
          f"measured={dl_dark / dl_bright:.1f}x "
          f"(dL* {dl_dark:.3f} vs {dl_bright:.3f})")

    h_need = required_spacing(X_DARK)
    report["required_h_at_005"] = h_need
    print(f"[3] JND-constrained local spacing at x=0.05: h <= {h_need:.4f}")
    grid_p = linspace(1.0, 2.0, 1001)
    p_star = next(p for p in grid_p if local_spacing(X_DARK, p) <= h_need)
    report["p_star_analytical"] = p_star
    print(f"    analytical exponent p* = {p_star:.3f} "
          f"(small-x limit p -> 3/2 from the x^(1/3) law)")

    fine = [(p, tier_dl(p, X_DARK), tier_dl(p, X_MID), tier_dl(p, X_BRIGHT))
            for p in [1.0 + 0.02 * i for i in range(11)]]
    cross = next(p for p, d, _, _ in fine if d <= JND)
    report["p_star_measured"] = cross
    print(f"    measured crossing (dense re-impl): p ~= {cross:.2f}")

    # design point p=1.3: endpoint and tier-local spacings
    p_design = 1.3
    xd = nodes(p_design)
    h0, hL = xd[1] - xd[0], xd[-1] - xd[-2]
    report["design"] = {
        "p": p_design,
        "h_first": h0, "h_last": hL, "endpoint_ratio": h0 / hL,
        "h_local_dark": local_spacing(X_DARK, p_design),
        "h_local_bright": local_spacing(X_BRIGHT, p_design),
        "local_ratio": local_spacing(X_DARK, p_design)
                     / local_spacing(X_BRIGHT, p_design),
        "dl_dark": tier_dl(p_design, X_DARK),
        "dl_mid": tier_dl(p_design, X_MID),
        "dl_bright": tier_dl(p_design, X_BRIGHT),
    }
    print(f"[4] design p={p_design}: endpoint spacing {h0:.5f}/{hL:.5f} "
          f"(ratio {h0 / hL:.3f}); tier-local ratio "
          f"{report['design']['local_ratio']:.3f}")

    # uniform failure numbers required by the brief
    xu = nodes(1.0)
    phys = max(abs(lut_sample_1d(x, xu, [f(v) for v in xu]) - f(x))
               for x in linspace(X_DARK - HALF_W, X_DARK + HALF_W, 401))
    report["uniform"] = {
        "h": xu[1] - xu[0],
        "dl_dark": dl_dark, "dl_mid": dl_mid_u,
        "dl_bright": dl_bright, "phys_dark": phys,
    }
    print(f"[5] uniform dark tier: dL*={dl_dark:.3f} > JND {JND}, "
          f"physical peak error={phys:.5f}, h={xu[1] - xu[0]:.4f} both ends")

    # cross-check against the C sweep CSV
    with open(HERE / "sweep.csv", newline="") as fh:
        sweep = list(csv.DictReader(fh))
    report["sweep"] = sweep
    c_uniform = next(r for r in sweep if r["p"] == "1.00")
    assert abs(float(c_uniform["dL_dark"]) - dl_dark) < 0.02
    c_design = next(r for r in sweep if r["p"] == "1.30")
    assert abs(float(c_design["dL_dark"]) - report["design"]["dl_dark"]) < 0.02
    print("[6] python re-implementation matches C sweep within 0.02 dL*")

    (HERE / "analysis.json").write_text(json.dumps(report, indent=2))
    render_html(report)
    print(f"wrote {HERE / 'analysis.json'} and {HERE / 'report.html'}")

# ----------------------------------------------------------------- HTML
PALETTE = {
    "blue": "#2a78d6", "orange": "#eb6834", "green": "#1baf7a",
    "red": "#e34948", "violet": "#6250d6",
}

def render_html(report):
    sweep = report["sweep"]
    p = [float(r["p"]) for r in sweep]
    dl_d = [float(r["dL_dark"]) for r in sweep]
    dl_m = [float(r["dL_mid"]) for r in sweep]
    dl_b = [float(r["dL_bright"]) for r in sweep]
    ns = [float(r["ns_lookup"]) for r in sweep]
    hd = [float(r["h_dark"]) for r in sweep]
    hb = [float(r["h_bright"]) for r in sweep]
    rr = [float(r["ratio"]) for r in sweep]

    panels = [chart_error(p, dl_d, dl_m, dl_b),
              chart_spacing(p, hd, hb, rr),
              chart_lookup(p, ns)]
    rows = "\n".join(
        f"<tr><td>{r['p']}</td><td>{float(r['h_dark']):.5f}</td>"
        f"<td>{float(r['h_bright']):.5f}</td>"
        f"<td>{float(r['ratio']):.4f}</td>"
        f"<td>{float(r['dL_dark']):.3f}</td>"
        f"<td>{float(r['dL_mid']):.3f}</td>"
        f"<td>{float(r['dL_bright']):.3f}</td>"
        f"<td>{float(r['ns_lookup']):.1f}</td></tr>"
        for r in sweep)
    d, u = report["design"], report["uniform"]
    html_doc = f"""<!doctype html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LUT 格点分布取舍报告</title>
<style>
:root {{
  color-scheme: light;
  --surface: #fcfcfb; --page: #f9f9f7;
  --ink: #0b0b0b; --ink-2: #52514e; --muted: #898781;
  --grid: #e1e0d9; --axis: #c3c2b7; --border: rgba(11,11,11,.10);
  --blue: {PALETTE['blue']}; --orange: {PALETTE['orange']};
  --green: {PALETTE['green']}; --red: {PALETTE['red']};
  --violet: {PALETTE['violet']};
}}
@media (prefers-color-scheme: dark) {{
  :root {{
    color-scheme: dark;
    --surface: #1a1a19; --page: #0d0d0d;
    --ink: #f0efec; --ink-2: #c3c2b7; --muted: #898781;
    --grid: #2c2c2a; --axis: #383835; --border: rgba(255,255,255,.10);
    --blue: #3987e5; --orange: #d95926; --green: #199e70;
    --red: #e66767; --violet: #9085e9;
  }}
}}
:root[data-theme="dark"] {{
  color-scheme: dark;
  --surface: #1a1a19; --page: #0d0d0d;
  --ink: #f0efec; --ink-2: #c3c2b7; --muted: #898781;
  --grid: #2c2c2a; --axis: #383835; --border: rgba(255,255,255,.10);
  --blue: #3987e5; --orange: #d95926; --green: #199e70;
  --red: #e66767; --violet: #9085e9;
}}
:root[data-theme="light"] {{
  color-scheme: light;
  --surface: #fcfcfb; --page: #f9f9f7;
  --ink: #0b0b0b; --ink-2: #52514e; --muted: #898781;
  --grid: #e1e0d9; --axis: #c3c2b7; --border: rgba(11,11,11,.10);
  --blue: {PALETTE['blue']}; --orange: {PALETTE['orange']};
  --green: {PALETTE['green']}; --red: {PALETTE['red']};
  --violet: {PALETTE['violet']};
}}
body {{ margin:0; background:var(--page); color:var(--ink);
  font:14px/1.6 system-ui,-apple-system,"Segoe UI",sans-serif; }}
main {{ max-width:980px; margin:0 auto; padding:32px 24px 64px; }}
h1 {{ font-size:22px; margin:0 0 4px; }}
h2 {{ font-size:17px; margin:36px 0 10px; }}
p, li {{ color:var(--ink-2); }}
.card {{ background:var(--surface); border:1px solid var(--border);
  border-radius:10px; padding:18px 18px 8px; margin-top:16px; }}
.card h3 {{ margin:0 0 2px; font-size:15px; }}
.card .sub {{ color:var(--muted); font-size:12.5px; margin:0 0 10px; }}
.kpis {{ display:flex; gap:12px; flex-wrap:wrap; margin-top:16px; }}
.kpi {{ flex:1 1 180px; background:var(--surface); border:1px solid var(--border);
  border-radius:10px; padding:12px 14px; }}
.kpi .v {{ font-size:22px; font-weight:650; }}
.kpi .l {{ color:var(--muted); font-size:12px; }}
.bad {{ color:var(--red); }} .good {{ color:var(--green); }}
table {{ border-collapse:collapse; width:100%; font-variant-numeric:tabular-nums;
  font-size:12.5px; margin-top:8px; }}
th, td {{ text-align:right; padding:5px 8px; border-bottom:1px solid var(--grid); }}
th:first-child, td:first-child {{ text-align:left; }}
th {{ color:var(--muted); font-weight:600; }}
button {{ font:inherit; border:1px solid var(--axis); background:var(--surface);
  color:var(--ink); border-radius:8px; padding:4px 12px; cursor:pointer; }}
svg {{ width:100%; height:auto; display:block; }}
.grid {{ stroke:var(--grid); stroke-width:1; }}
.axis {{ stroke:var(--axis); stroke-width:1; }}
.tick {{ fill:var(--muted); font-size:11px; }}
.llabel {{ font-size:12px; font-weight:600; }}
.line {{ fill:none; stroke-width:2; }}
.hit {{ fill:none; stroke:transparent; stroke-width:14px; pointer-events:stroke; }}
#tip {{ position:fixed; pointer-events:none; display:none; font-size:12px;
  background:var(--surface); border:1px solid var(--border); border-radius:8px;
  padding:7px 10px; box-shadow:0 4px 14px rgba(0,0,0,.18); z-index:5;
  font-variant-numeric:tabular-nums; }}
#tip b {{ display:block; margin-bottom:2px; }}
</style>
</head>
<body>
<main>
<h1>3D LUT 三线性插值：格点分布的感知误差 / 查表开销取舍</h1>
<p>N={N} 格点/轴，曲线 f(x)=x(1+k)/(x+k)，k={K}；感知度量 CIELAB L*（D65），JND ΔE={JND}。
<button id="theme" type="button">切换主题</button></p>

<div class="kpis">
  <div class="kpi"><div class="v bad">{u['dl_dark']:.2f}</div>
    <div class="l">均匀 LUT 暗部 ΔL*（阈值 {JND}，物理峰差 {u['phys_dark']:.4f}）</div></div>
  <div class="kpi"><div class="v good">{d['dl_dark']:.2f}</div>
    <div class="l">非均匀 p={d['p']} 暗部 ΔL*（三档全过）</div></div>
  <div class="kpi"><div class="v">{report['ratio_measured']:.0f}×</div>
    <div class="l">暗/亮感知误差实测倍数（解析式 {report['ratio_expr']:.0f}×）</div></div>
  <div class="kpi"><div class="v">1.8×</div>
    <div class="l">非均匀查表相对 FMA 快路径的固定开销</div></div>
</div>

<h2>1. 取舍曲线（C 实测扫参，hover 看数值）</h2>
{panels[0]}
{panels[1]}
{panels[2]}

<h2>2. 关键结论</h2>
<ul>
<li>均匀格点（p=1）暗部档 ΔL*={u['dl_dark']:.3f} &gt; {JND}，<b class="bad">不达标</b>；
物理峰值误差 {u['phys_dark']:.4f}，两端间距均为 h={u['h']:.4f}。</li>
<li>解析反解 p*≈{report['p_star_analytical']:.2f}（小 x 极限 3/2），实测 crossing p≈{report['p_star_measured']:.2f}；
取设计点 <b>p={d['p']}</b> 留安全边际。</li>
<li>p={d['p']} 时首尾格间距 {d['h_first']:.5f} / {d['h_last']:.5f}，
首尾比 {d['endpoint_ratio']:.3f}；两档中心局部间距比 {d['local_ratio']:.3f}，
暗/亮/中 ΔL* = {d['dl_dark']:.3f} / {d['dl_mid']:.3f} / {d['dl_bright']:.3f}。</li>
<li>查表开销是<b>进门费</b>：p&gt;1 后二分深度只由 N 决定，耗时不随 p 增长；
随 p 增长的是亮部间距（亮部被掏空），所以 p 取满足暗部达标的最小值。</li>
</ul>

<h2>3. 扫参数据表（表格视图）</h2>
<div class="card"><table>
<tr><th>p</th><th>h 暗端</th><th>h 亮端</th><th>首尾比</th>
<th>ΔL* 暗</th><th>ΔL* 中</th><th>ΔL* 亮</th><th>ns/次</th></tr>
{rows}
</table></div>
</main>
<div id="tip"></div>
<script>
document.getElementById('theme').onclick = () => {{
  const r = document.documentElement;
  r.dataset.theme = r.dataset.theme === 'dark' ? 'light' : 'dark';
}};
const tip = document.getElementById('tip');
document.querySelectorAll('svg.chart').forEach(svg => {{
  const meta = JSON.parse(svg.dataset.meta);
  const n = meta.p.length;
  svg.querySelectorAll('.hit').forEach(el => el.addEventListener('mousemove', e => {{
    const r = svg.getBoundingClientRect();
    const fx = (e.clientX - r.left) / r.width * {W};
    const frac = Math.min(1, Math.max(0, (fx - {ML}) / ({W} - {ML} - {MR})));
    const i = Math.round(frac * (n - 1));
    tip.style.display = 'block';
    tip.style.left = (e.clientX + 14) + 'px';
    tip.style.top = (e.clientY + 14) + 'px';
    tip.innerHTML = '<b>p=' + meta.p[i].toFixed(2) + '</b>' +
      meta.series.map(s => s[2] + ': ' + s[0][i].toFixed(s[3])).join('<br>');
  }}));
  svg.addEventListener('mouseleave', () => tip.style.display = 'none');
}});
</script>
</body></html>"""
    (HERE / "report.html").write_text(html_doc)

# ------------------------------------------------------------ SVG helpers
W, H = 900, 300
ML, MR, MT, MB = 64, 16, 18, 34

def _scales(x, ysets, log_y=False):
    yall = [v for seq in ysets for v in seq]
    if log_y:
        ymin, ymax = min(yall), max(yall)
        lo_g, hi_g = math.floor(math.log10(ymin)), math.ceil(math.log10(ymax))
        ticks = [10.0 ** e for e in range(lo_g, hi_g + 1)]
        def sy(v):
            t = ((math.log10(v) - math.log10(ymin)) /
                 (math.log10(ymax) - math.log10(ymin)))
            return MT + (1 - t) * (H - MT - MB)
    else:
        pad = (max(yall) - min(yall)) * 0.08 or 1.0
        ymin, ymax = min(yall) - pad, max(yall) + pad
        ticks = linspace(ymin, ymax, 5)
        def sy(v):
            return MT + (1 - (v - ymin) / (ymax - ymin)) * (H - MT - MB)
    def sx(v):
        return ML + (v - x[0]) / (x[-1] - x[0]) * (W - ML - MR)
    return sx, sy, ticks

def _svg_frame(sx, sy, xticks, yticks, xlabel, ylabel, log_y=False):
    out = [f'<line class="axis" x1="{ML}" y1="{H-MB}" x2="{W-MR}" y2="{H-MB}"/>',
           f'<line class="axis" x1="{ML}" y1="{MT}" x2="{ML}" y2="{H-MB}"/>']
    for t in yticks:
        y = sy(t)
        out.append(f'<line class="grid" x1="{ML}" y1="{y:.1f}" x2="{W-MR}" y2="{y:.1f}"/>')
        label = ("%g" % t) if not log_y else ("%.3f" % t).rstrip("0").rstrip(".")
        out.append(f'<text class="tick" x="{ML-7}" y="{y+3.5:.1f}" text-anchor="end">{label}</text>')
    for t in xticks:
        out.append(f'<text class="tick" x="{sx(t):.1f}" y="{H-MB+16}" text-anchor="middle">{t:g}</text>')
    out.append(f'<text class="tick" x="{(ML+W-MR)/2}" y="{H-4}" text-anchor="middle">{html.escape(xlabel)}</text>')
    out.append(f'<text class="tick" x="16" y="{(MT+H-MB)/2}" text-anchor="middle" '
               f'transform="rotate(-90 16 {(MT+H-MB)/2})">{html.escape(ylabel)}</text>')
    return "".join(out)

def _line(x, y, sx, sy, color):
    pts = " ".join(f"{sx(vx):.1f},{sy(vy):.1f}" for vx, vy in zip(x, y))
    return f'<polyline class="line" stroke="var({color})" points="{pts}"/>'

def _end_label(x, y, sx, sy, color, text, digits=3, dy=0.0):
    return (f'<text class="llabel" x="{sx(x[-1])-6:.1f}" y="{sy(y[-1])+dy:.1f}" '
            f'text-anchor="end" fill="var({color})">{html.escape(text)} {y[-1]:.{digits}f}</text>')

def _point_label(x, y, idx, sx, sy, color, text, dy=0.0):
    """interior label for series whose right end collides with another"""
    return (f'<text class="llabel" x="{sx(x[idx]):.1f}" y="{sy(y[idx])+dy:.1f}" '
            f'text-anchor="middle" fill="var({color})">{html.escape(text)}</text>')

def _hitline(x, y, sx, sy):
    pts = " ".join(f"{sx(vx):.1f},{sy(vy):.1f}" for vx, vy in zip(x, y))
    return f'<polyline class="hit" points="{pts}"/>'

def chart_error(p, d, m, b):
    sx, sy, yt = _scales(p, [d, m, b, [JND] * len(p)])
    xt = [1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0]
    frame = _svg_frame(sx, sy, xt, yt, "格点律指数 p  (xᵢ=(i/16)ᵖ)", "ΔL* 感知误差")
    jnd_y = sy(JND)
    lines = [
        f'<line x1="{ML}" y1="{jnd_y:.1f}" x2="{W-MR}" y2="{jnd_y:.1f}" '
        f'stroke="var(--red)" stroke-width="2" stroke-dasharray="6 5"/>',
        f'<text class="llabel" x="{ML+8}" y="{jnd_y-7:.1f}" fill="var(--red)">JND = {JND}</text>',
        _line(p, d, sx, sy, "--blue"), _line(p, m, sx, sy, "--orange"),
        _line(p, b, sx, sy, "--green"),
        _end_label(p, d, sx, sy, "--blue", "暗部 L=0.05", dy=-9),
        _point_label(p, m, 12, sx, sy, "--orange", "中部 L=0.18", dy=-10),
        _end_label(p, b, sx, sy, "--green", "亮部 L=0.50", dy=14)]
    meta = json.dumps({"p": p, "series": [(d, "--blue", "暗部", 3),
                                          (m, "--orange", "中部", 3),
                                          (b, "--green", "亮部", 3)]})
    return (f'<div class="card"><h3>① 感知误差 vs 格点律 p</h3>'
            f'<p class="sub">p=1 即均匀格点：暗部 3.00 超 JND；p≈1.1 起暗部达标，之后边际收益迅速递减。</p>'
            f'<svg class="chart" viewBox="0 0 {W} {H}" data-meta="{html.escape(meta)}">'
            + frame + "".join(lines) + _hitline(p, d, sx, sy) + "</svg></div>")

def chart_spacing(p, hd, hb, rr):
    sx, sy, yt = _scales(p, [hd, hb], log_y=True)
    xt = [1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0]
    frame = _svg_frame(sx, sy, xt, yt, "格点律指数 p", "格间距 h（对数轴）", log_y=True)
    lines = [_line(p, hd, sx, sy, "--blue"), _line(p, hb, sx, sy, "--orange"),
             _end_label(p, hd, sx, sy, "--blue", "暗端 h₀", 5, dy=-8),
             _end_label(p, hb, sx, sy, "--orange", "亮端 hₗ", 4, dy=14)]
    meta = json.dumps({"p": p, "series": [(hd, "--blue", "h 暗端", 5),
                                          (hb, "--orange", "h 亮端", 5),
                                          (rr, "--violet", "首尾比", 4)]})
    return (f'<div class="card"><h3>② 格间距与首尾比 vs p（对数轴）</h3>'
            f'<p class="sub">p 越大暗端越密、亮端越疏；首尾比（紫，见数据表与 hover）随 p 单调恶化，'
            f'这是 p 不能无限加大的真实代价。</p>'
            f'<svg class="chart" viewBox="0 0 {W} {H}" data-meta="{html.escape(meta)}">'
            + frame + "".join(lines) + _hitline(p, hd, sx, sy) + "</svg></div>")

def chart_lookup(p, ns):
    sx, sy, yt = _scales(p, [ns])
    xt = [1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0]
    frame = _svg_frame(sx, sy, xt, yt, "格点律指数 p", "单次查表 ns（3 次取最小值）")
    pts = " ".join(f"{sx(vx):.1f},{sy(vy):.1f}" for vx, vy in zip(p, ns))
    dots = "".join(f'<circle cx="{sx(vx):.1f}" cy="{sy(vy):.1f}" r="3" fill="var(--violet)"/>'
                   for vx, vy in zip(p, ns))
    meta = json.dumps({"p": p, "series": [(ns, "--violet", "通用二分路径", 1)]})
    return (f'<div class="card"><h3>③ 查表开销 vs p</h3>'
            f'<p class="sub">通用路径（二分定位 + 8 角点混合）约 30–35 ns，与 p 无关；'
            f'均匀 FMA 快路径 17.2 ns 仅在 p=1 可用——非均匀是约 1.8× 的固定进门费。</p>'
            f'<svg class="chart" viewBox="0 0 {W} {H}" data-meta="{html.escape(meta)}">'
            + frame + f'<polyline class="line" stroke="var(--violet)" points="{pts}"/>' + dots
            + _hitline(p, ns, sx, sy) + "</svg></div>")

if __name__ == "__main__":
    main()
