#!/usr/bin/env python3
"""Analyze bench_out.txt: invert the skip criterion and derive a per-tier
skip strategy that fits the 3 ms post-process budget.

Stdlib only. Run with: python3 -I analyze.py [bench_out.txt]
Outputs: summary.json (machine) and tradeoff.html (two-facet chart + table).
"""
import json
import sys
from itertools import combinations

BUDGET_MS = 3.0
STAGES = ("tonemap", "bloom", "dof")
SKIP_CANDIDATES = ("bloom", "dof")   # tonemap's fate is decided by the criterion
VARIANT_SKIPS = {
    "full": (),
    "skip_bloom": ("bloom",),
    "skip_dof": ("dof",),
    "skip_bloom_dof": ("bloom", "dof"),
    "skip_tonemap": ("tonemap",),
}


def parse(path):
    threshold = None
    times = {}        # (tier, stage) -> ms
    diffs = {}        # (tier, variant) -> dict
    tiers = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("threshold_rmse="):
                threshold = float(line.split("=")[1].split()[0])
            elif line.startswith("CSV_TIME,"):
                _, tier, stage, ms = line.split(",")
                if stage == "stage":
                    continue
                times[(tier, stage)] = float(ms)
                if tier not in tiers:
                    tiers.append(tier)
            elif line.startswith("CSV_DIFF,"):
                _, tier, variant, rmse, psnr, mad, maxd, aff, skip = line.split(",")
                if variant == "variant":
                    continue
                diffs[(tier, variant)] = {
                    "rmse": float(rmse), "psnr": float(psnr),
                    "mad": float(mad), "maxd": float(maxd),
                    "affected": float(aff), "skippable": bool(int(skip)),
                }
    return threshold, tiers, times, diffs


def variant_time(times, tier, skips):
    total = times[(tier, "total")]
    return total - sum(times[(tier, s)] for s in skips)


def analyze(threshold, tiers, times, diffs):
    report = {"threshold_rmse": threshold, "budget_ms": BUDGET_MS, "tiers": {}}

    # ---- per-stage skippability from the measured single-skip variants ----
    stage_verdict = {}
    for s in STAGES:
        v = "skip_" + s
        d = diffs[(tiers[0], v)]   # diff is resolution-independent in this scene
        stage_verdict[s] = {
            "rmse": d["rmse"], "psnr": d["psnr"],
            "skippable": d["rmse"] <= threshold,
        }

    # ---- invert the criterion: largest subset whose diff stays under threshold ----
    for tier in tiers:
        subsets = []
        for r in range(len(SKIP_CANDIDATES) + 1):
            for combo in combinations(SKIP_CANDIDATES, r):
                name = "full" if not combo else "skip_" + "_".join(combo)
                d = diffs[(tier, name)]
                subsets.append({
                    "skips": list(combo),
                    "variant": name,
                    "rmse": d["rmse"], "psnr": d["psnr"],
                    "saved_ms": sum(times[(tier, s)] for s in combo),
                    "result_ms": variant_time(times, tier, combo),
                    "within_threshold": d["rmse"] <= threshold,
                    "within_budget": variant_time(times, tier, combo) <= BUDGET_MS,
                })
        feasible = [s for s in subsets if s["within_threshold"]]
        k_star = max(len(s["skips"]) for s in feasible)

        # ---- strategy: minimum skips that meet the budget, diff capped ----
        ranked = sorted(
            (s for s in feasible),
            key=lambda s: (len(s["skips"]), s["rmse"]),
        )
        chosen = next((s for s in ranked if s["within_budget"]), None)
        if chosen is None:
            chosen = max(feasible, key=lambda s: s["saved_ms"])

        # ordered skip sequence: most time saved first; record each step's saving
        order = sorted(chosen["skips"], key=lambda s: -times[(tier, s)])
        sequence = [
            {"step": s, "saved_ms": times[(tier, s)],
             "cumulative_ms": sum(times[(tier, x)] for x in order[: i + 1])}
            for i, s in enumerate(order)
        ]

        report["tiers"][tier] = {
            "stage_time_ms": {s: times[(tier, s)] for s in STAGES},
            "full_ms": times[(tier, "total")],
            "subsets": subsets,
            "max_skippable": k_star,
            "chosen": chosen,
            "sequence": sequence,
            "budget_met": chosen["within_budget"],
        }
    report["stage_verdict"] = stage_verdict
    return report


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "bench_out.txt"
    threshold, tiers, times, diffs = parse(src)
    report = analyze(threshold, tiers, times, diffs)
    with open("summary.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    with open("tradeoff.html", "w", encoding="utf-8") as f:
        f.write(render_html(report, tiers))

    print(f"threshold RMSE = {threshold:.4f}   budget = {BUDGET_MS:.1f} ms\n")
    print("stage skippability (single-skip RMSE vs threshold):")
    for s in STAGES:
        v = report["stage_verdict"][s]
        print(f"  {s:8s} rmse={v['rmse']:.5f}  skippable={v['skippable']}")
    for tier in tiers:
        t = report["tiers"][tier]
        c = t["chosen"]
        print(f"\n{tier}: full={t['full_ms']:.3f} ms  max skippable stages={t['max_skippable']}")
        for s in t["subsets"]:
            print("  {:14s} result={:6.3f} ms  saved={:6.3f}  rmse={:.5f}  budget={} threshold={}".format(
                s["variant"], s["result_ms"], s["saved_ms"], s["rmse"],
                "ok" if s["within_budget"] else "OVER",
                "ok" if s["within_threshold"] else "OVER"))
        print("  strategy: skip {} -> {:.3f} ms, rmse {:.5f}, budget_met={}".format(
            c["skips"] or "nothing", c["result_ms"], c["rmse"], t["budget_met"]))
        for step in t["sequence"]:
            print(f"    order: skip {step['step']:7s} saves {step['saved_ms']:.3f} ms "
                  f"(cumulative {step['cumulative_ms']:.3f})")
    print("\nwrote summary.json and tradeoff.html")


def render_html(report, tiers):
    import math
    thr = report["threshold_rmse"]
    psnr = 20.0 * math.log10(1.0 / thr)
    return (TEMPLATE
            .replace("__DATA__", json.dumps(report, ensure_ascii=False))
            .replace("__THR__", f"{thr:.4f}")
            .replace("__PSNR__", f"{psnr:.1f}"))


TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>后处理跳过策略：画面差异 × 耗时</title>
<style>
.viz-root {
  color-scheme: light;
  --surface-1: #fcfcfb; --page: #f9f9f7;
  --text-primary: #0b0b0b; --text-secondary: #52514e; --muted: #898781;
  --grid: #e1e0d9; --axis: #c3c2b7; --border: rgba(11,11,11,0.10);
  --s1: #2a78d6; --s2: #eb6834; --s3: #1baf7a;
  --threshold: #6250d6; --critical: #d03b3b; --good: #006300;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) .viz-root {
    color-scheme: dark;
    --surface-1: #1a1a19; --page: #0d0d0d;
    --text-primary: #f0efec; --text-secondary: #c3c2b7; --muted: #898781;
    --grid: #2c2c2a; --axis: #383835; --border: rgba(255,255,255,0.10);
    --s1: #3987e5; --s2: #d95926; --s3: #199e70;
    --threshold: #9085e9; --critical: #e66767; --good: #0ca30c;
  }
}
:root[data-theme="dark"] .viz-root {
  color-scheme: dark;
  --surface-1: #1a1a19; --page: #0d0d0d;
  --text-primary: #f0efec; --text-secondary: #c3c2b7; --muted: #898781;
  --grid: #2c2c2a; --axis: #383835; --border: rgba(255,255,255,0.10);
  --s1: #3987e5; --s2: #d95926; --s3: #199e70;
  --threshold: #9085e9; --critical: #e66767; --good: #0ca30c;
}
.viz-root { font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
  background: var(--page); color: var(--text-primary); margin: 0; padding: 24px; }
.card { background: var(--surface-1); border: 1px solid var(--border);
  border-radius: 10px; padding: 20px; max-width: 1080px; margin: 0 auto 20px; }
h1 { font-size: 19px; margin: 0 0 4px; }
h2 { font-size: 15px; margin: 0 0 14px; color: var(--text-secondary); font-weight: 600; }
.sub { color: var(--text-secondary); font-size: 13px; margin: 0 0 16px; }
.row { display: flex; flex-wrap: wrap; gap: 24px; align-items: center; }
.legend { display: flex; gap: 18px; font-size: 13px; color: var(--text-secondary); flex-wrap: wrap; }
.legend span { display: inline-flex; align-items: center; gap: 6px; }
.sw { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }
.sw.line { height: 3px; border-radius: 2px; width: 18px; }
button.theme { margin-left: auto; font-size: 12px; padding: 5px 10px; border-radius: 6px;
  border: 1px solid var(--axis); background: var(--surface-1); color: var(--text-secondary); cursor: pointer; }
svg { width: 100%; height: auto; display: block; }
.axis { stroke: var(--axis); stroke-width: 1; }
.grid { stroke: var(--grid); stroke-width: 1; }
.tick { fill: var(--muted); font-size: 11px; font-variant-numeric: tabular-nums; }
.axis-label { fill: var(--text-secondary); font-size: 12px; }
.pt { cursor: pointer; }
.dlabel { font-size: 10.5px; fill: var(--text-secondary); font-variant-numeric: tabular-nums; }
.facet-title { font-size: 13px; fill: var(--text-primary); font-weight: 600; }
.bar-label { font-size: 11px; font-variant-numeric: tabular-nums; fill: var(--text-primary); }
.over { fill: var(--critical); font-size: 11px; font-weight: 600; }
table { border-collapse: collapse; width: 100%; font-size: 12.5px; font-variant-numeric: tabular-nums; }
th, td { text-align: right; padding: 7px 10px; border-bottom: 1px solid var(--grid); }
th:first-child, td:first-child { text-align: left; }
th { color: var(--text-secondary); font-weight: 600; }
td.bad { color: var(--critical); font-weight: 600; }
td.ok { color: var(--good); }
#tip { position: fixed; pointer-events: none; background: var(--surface-1);
  border: 1px solid var(--border); border-radius: 8px; padding: 8px 11px;
  font-size: 12px; color: var(--text-primary); box-shadow: 0 4px 16px rgba(0,0,0,.18);
  opacity: 0; transition: opacity .08s; max-width: 260px; z-index: 10; }
#tip b { font-size: 12.5px; } #tip .m { color: var(--text-secondary); }
</style>
</head>
<body class="viz-root">
<div class="card">
  <div class="row">
    <div>
      <h1>后处理跳过策略：画面差异 × 耗时 两条曲线</h1>
      <p class="sub">判据：跳过 S 后显示空间 RMSE ≤ 感知阈值 __THR__（PSNR ≥ __PSNR__ dB）；耗时预算 3.0 ms。数据来自 bench 实测中位数。</p>
    </div>
    <button class="theme" onclick="toggleTheme()">切换深浅色</button>
  </div>
  <div class="legend">
    <span><i class="sw" style="background:var(--s1)"></i>L1 384×216</span>
    <span><i class="sw" style="background:var(--s2)"></i>L2 552×311</span>
    <span><i class="sw" style="background:var(--s3)"></i>L3 640×360</span>
    <span><i class="sw line" style="background:var(--threshold)"></i>感知阈值 RMSE</span>
    <span><i class="sw line" style="background:var(--critical)"></i>3 ms 预算线</span>
  </div>
  <h2 style="margin-top:18px">曲线一：省下的耗时（横轴）vs 跳过后画面差异 RMSE（纵轴）</h2>
  <div id="chart1"></div>
  <h2 style="margin-top:24px">曲线二：各跳过方案的管线剩余耗时（虚线为 3 ms 预算）</h2>
  <div id="chart2"></div>
</div>
<div class="card">
  <h2>实测明细表</h2>
  <div id="table"></div>
</div>
<div id="tip"></div>
<script>
const R = __DATA__;
const TIER_COLOR = ["var(--s1)", "var(--s2)", "var(--s3)"];
const tiers = Object.keys(R.tiers);
const tip = document.getElementById("tip");
function showTip(e, html) { tip.innerHTML = html; tip.style.opacity = 1; moveTip(e); }
function moveTip(e) { tip.style.left = (e.clientX + 14) + "px"; tip.style.top = (e.clientY + 14) + "px"; }
function hideTip() { tip.style.opacity = 0; }
function toggleTheme() {
  const r = document.documentElement;
  r.dataset.theme = (r.dataset.theme === "dark") ? "light" : "dark";
}
const VLABEL = {full:"不跳", skip_bloom:"跳泛光", skip_dof:"跳景深", skip_bloom_dof:"双跳", skip_tonemap:"跳色调映射"};
const VFULL = {full:"不跳", skip_bloom:"跳泛光", skip_dof:"跳景深", skip_bloom_dof:"跳泛光+景深", skip_tonemap:"跳色调映射"};

/* ---- chart 1: saved time (x) vs RMSE (y), one polyline per tier ---- */
(function () {
  const W = 1020, H = 380, M = {l: 64, r: 28, t: 18, b: 52};
  const pw = W - M.l - M.r, ph = H - M.t - M.b;
  let xmax = 0, ymax = R.threshold_rmse * 1.15;
  tiers.forEach(t => R.tiers[t].subsets.forEach(s => { xmax = Math.max(xmax, s.saved_ms); }));
  ymax = Math.max(ymax, 0.012 * 1.15);
  const x = v => M.l + v / xmax * pw;
  const y = v => M.t + ph - v / ymax * ph;
  let svg = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="saved time versus RMSE">`;
  for (let i = 0; i <= 5; i++) {
    const xv = xmax * i / 5, xp = x(xv);
    svg += `<line class="grid" x1="${xp}" y1="${M.t}" x2="${xp}" y2="${M.t+ph}"/>`;
    svg += `<text class="tick" x="${xp}" y="${M.t+ph+18}" text-anchor="middle">${xv.toFixed(1)}</text>`;
  }
  for (let i = 0; i <= 4; i++) {
    const yv = ymax * i / 4, yp = y(yv);
    svg += `<line class="grid" x1="${M.l}" y1="${yp}" x2="${M.l+pw}" y2="${yp}"/>`;
    svg += `<text class="tick" x="${M.l-8}" y="${yp+4}" text-anchor="end">${yv.toFixed(4)}</text>`;
  }
  svg += `<line class="axis" x1="${M.l}" y1="${M.t+ph}" x2="${M.l+pw}" y2="${M.t+ph}"/>`;
  svg += `<line class="axis" x1="${M.l}" y1="${M.t}" x2="${M.l}" y2="${M.t+ph}"/>`;
  const yt = y(R.threshold_rmse);
  svg += `<line x1="${M.l}" y1="${yt}" x2="${M.l+pw}" y2="${yt}" stroke="var(--threshold)" stroke-width="2" stroke-dasharray="7 5"/>`;
  svg += `<text class="dlabel" x="${M.l+pw}" y="${yt-6}" text-anchor="end" fill="var(--threshold)">感知阈值 ${R.threshold_rmse.toFixed(4)}</text>`;
  svg += `<text class="axis-label" x="${M.l+pw/2}" y="${H-12}" text-anchor="middle">省下的耗时 (ms)</text>`;
  svg += `<text class="axis-label" transform="translate(18,${M.t+ph/2}) rotate(-90)" text-anchor="middle">跳过后画面差异 RMSE</text>`;
  tiers.forEach((t, ti) => {
    const pts = R.tiers[t].subsets.filter(s => !s.skips.includes("tonemap"))
      .slice().sort((a,b) => a.saved_ms - b.saved_ms);
    const poly = pts.map(s => `${x(s.saved_ms)},${y(s.rmse)}`).join(" ");
    svg += `<polyline points="${poly}" fill="none" stroke="${TIER_COLOR[ti]}" stroke-width="2"/>`;
    /* direct-label only the two endpoints; stagger L2 below, L3 above to avoid collisions */
    const dyEnd = ti === 1 ? 18 : -10;
    pts.forEach((s, i) => {
      const cx = x(s.saved_ms), cy = y(s.rmse);
      svg += `<circle class="pt" cx="${cx}" cy="${cy}" r="7" fill="${TIER_COLOR[ti]}" stroke="var(--surface-1)" stroke-width="2"
        data-tip='${JSON.stringify(s).replace(/'/g,"&#39;")}' data-label="${VLABEL[s.variant]}" data-tier="${t}"/>`;
      const isEnd = (i === 0 || i === pts.length - 1);
      if (isEnd && !(i === 0 && ti > 0))
        svg += `<text class="dlabel" x="${cx}" y="${cy + (i === 0 ? -10 : dyEnd)}" text-anchor="middle">${VLABEL[s.variant]}</text>`;
    });
  });
  svg += `</svg>`;
  document.getElementById("chart1").innerHTML = svg;
})();

/* ---- chart 2: small multiples, remaining time per variant, budget line ---- */
(function () {
  const fw = 330, fh = 250, M = {l: 46, r: 12, t: 30, b: 46};
  const pw = fw - M.l - M.r, ph = fh - M.t - M.b;
  let ymax = 0;
  tiers.forEach(t => R.tiers[t].subsets.forEach(s => ymax = Math.max(ymax, s.result_ms)));
  ymax = Math.ceil(ymax * 1.1);
  const y = v => M.t + ph - v / ymax * ph;
  let svg = `<svg viewBox="0 0 ${fw*3} ${fh}" role="img" aria-label="remaining pipeline time per variant">`;
  tiers.forEach((t, ti) => {
    const ox = ti * fw;
    const pts = R.tiers[t].subsets.filter(s => !s.skips.includes("tonemap"))
      .slice().sort((a,b) => a.saved_ms - b.saved_ms);
    const bw = pw / pts.length * 0.62, gap = pw / pts.length;
    svg += `<text class="facet-title" x="${ox+M.l}" y="18">${t}（全量 ${R.tiers[t].full_ms.toFixed(2)} ms）</text>`;
    for (let i = 0; i <= 4; i++) {
      const yv = ymax * i / 4, yp = ox + M.l, yvpx = y(yv);
      svg += `<line class="grid" x1="${yp}" y1="${yvpx}" x2="${ox+M.l+pw}" y2="${yvpx}"/>`;
      svg += `<text class="tick" x="${ox+M.l-6}" y="${yvpx+4}" text-anchor="end">${yv.toFixed(1)}</text>`;
    }
    const yb = y(R.budget_ms);
    svg += `<line x1="${ox+M.l}" y1="${yb}" x2="${ox+M.l+pw}" y2="${yb}" stroke="var(--critical)" stroke-width="2" stroke-dasharray="6 4"/>`;
    svg += `<text class="dlabel" x="${ox+M.l+pw}" y="${yb-5}" text-anchor="end" fill="var(--critical)">预算 3.0 ms</text>`;
    pts.forEach((s, i) => {
      const bx = ox + M.l + i * gap + (gap - bw) / 2;
      const by = y(s.result_ms), bh = M.t + ph - by;
      svg += `<rect class="pt" x="${bx}" y="${by}" width="${bw}" height="${bh}" rx="4" fill="${TIER_COLOR[ti]}"
        data-tip='${JSON.stringify(s).replace(/'/g,"&#39;")}' data-label="${VLABEL[s.variant]}" data-tier="${t}"/>`;
      svg += `<text class="bar-label" x="${bx+bw/2}" y="${by-5}" text-anchor="middle">${s.result_ms.toFixed(2)}</text>`;
      if (!s.within_budget)
        svg += `<text class="over" x="${bx+bw/2}" y="${M.t+ph+16}" text-anchor="middle">超预算</text>`;
      svg += `<text class="tick" x="${bx+bw/2}" y="${M.t+ph+(s.within_budget?16:32)}" text-anchor="middle">${VLABEL[s.variant]}</text>`;
    });
    svg += `<line class="axis" x1="${ox+M.l}" y1="${M.t+ph}" x2="${ox+M.l+pw}" y2="${M.t+ph}"/>`;
  });
  svg += `<text class="axis-label" transform="translate(14,${fh/2}) rotate(-90)" text-anchor="middle">剩余耗时 (ms)</text>`;
  svg += `</svg>`;
  document.getElementById("chart2").innerHTML = svg;
})();

/* ---- table view ---- */
(function () {
  let h = `<table><thead><tr><th>负载档</th><th>方案</th><th>剩余耗时 ms</th><th>省下 ms</th><th>RMSE</th><th>PSNR dB</th><th>阈值内</th><th>预算内</th></tr></thead><tbody>`;
  tiers.forEach(t => R.tiers[t].subsets.filter(s => !s.skips.includes("tonemap")).forEach(s => {
    h += `<tr><td>${t}</td><td>${VFULL[s.variant]}</td><td>${s.result_ms.toFixed(3)}</td>
      <td>${s.saved_ms.toFixed(3)}</td><td>${s.rmse.toFixed(5)}</td><td>${s.psnr.toFixed(1)}</td>
      <td class="${s.within_threshold?"ok":"bad"}">${s.within_threshold?"是":"否"}</td>
      <td class="${s.within_budget?"ok":"bad"}">${s.within_budget?"是":"否"}</td></tr>`;
  }));
  h += `</tbody></table>`;
  document.getElementById("table").innerHTML = h;
})();

/* ---- hover tooltips ---- */
document.querySelectorAll(".pt").forEach(el => {
  el.addEventListener("mousemove", e => {
    const s = JSON.parse(el.dataset.tip.replace(/&#39;/g, "'"));
    showTip(e, `<b>${el.dataset.tier} · ${VFULL[s.variant]}</b><br>
      <span class="m">剩余耗时</span> ${s.result_ms.toFixed(3)} ms<br>
      <span class="m">省下</span> ${s.saved_ms.toFixed(3)} ms<br>
      <span class="m">RMSE</span> ${s.rmse.toFixed(5)}（PSNR ${s.psnr.toFixed(1)} dB）<br>
      <span class="m">阈值内 / 预算内</span> ${s.within_threshold?"是":"否"} / ${s.within_budget?"是":"否"}`);
  });
  el.addEventListener("mouseleave", hideTip);
});
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
