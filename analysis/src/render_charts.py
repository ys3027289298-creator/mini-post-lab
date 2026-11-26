#!/usr/bin/env python3 -I
"""Render accuracy/bandwidth trade-off curves from depth_sweep.csv (stdlib only)."""
import csv
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out")
TOL = 1.0 / 255.0

rows = list(csv.DictReader(open(os.path.join(OUT, "depth_sweep.csv"))))
depths = sorted({int(r["depth"]) for r in rows})
tiers = ["dark", "mid", "bright"]
curves = ["reinhard", "aces"]

PALETTE = {"dark": "#4C78A8", "mid": "#F58518", "bright": "#E45756"}


def chart(curve, path):
    Wd, Hd = 760, 460
    ml, mr, mt, mb = 78, 200, 48, 58
    pw, ph = Wd - ml - mr, Hd - mt - mb
    data = {(r["tier"], int(r["depth"])): float(r["max_abs_diff"])
            for r in rows if r["curve"] == curve}
    ymax = max(data.values()) * 1.08
    def X(b): return ml + (b - 8) / (32 - 8) * pw
    def Y(v): return mt + ph - v / ymax * ph
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{Wd}" height="{Hd}" font-family="sans-serif">']
    s.append(f'<rect width="{Wd}" height="{Hd}" fill="white"/>')
    s.append(f'<text x="{Wd/2}" y="26" text-anchor="middle" font-size="17" font-weight="bold">'
             f'preview-export max |ΔL| vs readback bit depth ({curve})</text>')
    # y grid + labels (log-ish linear)
    for g in range(6):
        v = ymax * g / 5
        yy = Y(v)
        s.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{ml+pw}" y2="{yy:.1f}" stroke="#eee"/>')
        s.append(f'<text x="{ml-8}" y="{yy+4:.1f}" text-anchor="end" font-size="11" fill="#555">{v:.4f}</text>')
    # tolerance line
    yt = Y(TOL)
    s.append(f'<line x1="{ml}" y1="{yt:.1f}" x2="{ml+pw}" y2="{yt:.1f}" stroke="#2ca02c" '
             f'stroke-dasharray="6 4" stroke-width="1.6"/>')
    s.append(f'<text x="{ml+6}" y="{yt-6:.1f}" font-size="12" fill="#2ca02c">'
             f'tolerance = 1/255 = {TOL:.4f} (one final quantization level)</text>')
    # x axis
    for b in depths:
        xx = X(b)
        s.append(f'<line x1="{xx:.1f}" y1="{mt+ph}" x2="{xx:.1f}" y2="{mt+ph+5}" stroke="#888"/>')
        s.append(f'<text x="{xx:.1f}" y="{mt+ph+20}" text-anchor="middle" font-size="11">{b}</text>')
    s.append(f'<text x="{ml+pw/2}" y="{Hd-12}" text-anchor="middle" font-size="13">readback buffer bit depth b</text>')
    s.append(f'<text x="20" y="{mt+ph/2}" font-size="13" transform="rotate(-90 20 {mt+ph/2})" '
             f'text-anchor="middle">max |preview &#8722; export| in tone-mapped L</text>')
    # series
    for tier in tiers:
        pts = [(X(b), Y(data[(tier, b)])) for b in depths]
        p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        s.append(f'<polyline fill="none" stroke="{PALETTE[tier]}" stroke-width="2" points="{p}"/>')
        for x, y in pts:
            s.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="3.4" fill="{PALETTE[tier]}"/>')
    # legend with bandwidth axis
    lx = ml + pw + 18
    s.append(f'<text x="{lx}" y="{mt+4}" font-size="12" font-weight="bold">brightness tier</text>')
    for k, tier in enumerate(tiers):
        yy = mt + 26 + k * 22
        s.append(f'<line x1="{lx}" y1="{yy}" x2="{lx+22}" y2="{yy}" stroke="{PALETTE[tier]}" stroke-width="2"/>')
        s.append(f'<text x="{lx+28}" y="{yy+4}" font-size="12">{tier}</text>')
    s.append(f'<text x="{lx}" y="{mt+108}" font-size="12" font-weight="bold">relative readback bandwidth</text>')
    s.append(f'<text x="{lx}" y="{mt+126}" font-size="11" fill="#555">(bytes/pixel/channel, vs 8-bit)</text>')
    for k, b in enumerate(depths):
        yy = mt + 146 + k * 20
        rel = b / 8.0
        s.append(f'<text x="{lx}" y="{yy}" font-size="11">{b:2d}-bit : {b/8:.2f}x '
                 f'({"same" if b == 8 else "x%.2f" % rel})</text>')
    s.append("</svg>")
    open(path, "w").write("\n".join(s))


def bandwidth_chart(path):
    Wd, Hd = 760, 460
    ml, mr, mt, mb = 78, 170, 48, 58
    pw, ph = Wd - ml - mr, Hd - mt - mb
    # one point per (tier,curve): measured minimum depth
    mins = {}
    for r in rows:
        key = (r["tier"], r["curve"])
        if r["within_one_level"] == "1":
            mins.setdefault(key, int(r["depth"]))
    bs = sorted({v for v in mins.values()})
    bmin, bmax = min(bs), max(bs)
    def X(b): return ml + (b - bmin) / (bmax - bmin) * pw
    def Y(v): return mt + ph - min(v, 1.0) / 1.05 * ph
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{Wd}" height="{Hd}" font-family="sans-serif">']
    s.append(f'<rect width="{Wd}" height="{Hd}" fill="white"/>')
    s.append(f'<text x="{Wd/2}" y="26" text-anchor="middle" font-size="17" font-weight="bold">'
             f'trade-off: minimum depth for ≤1-level error vs bandwidth cost</text>')
    for g in range(6):
        v = 1.05 * g / 5
        yy = Y(v)
        s.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{ml+pw}" y2="{yy:.1f}" stroke="#eee"/>')
        s.append(f'<text x="{ml-8}" y="{yy+4:.1f}" text-anchor="end" font-size="11" fill="#555">{v:.2f}x</text>')
    for b in range(bmin, bmax + 1):
        xx = X(b)
        s.append(f'<line x1="{xx:.1f}" y1="{mt+ph}" x2="{xx:.1f}" y2="{mt+ph+5}" stroke="#888"/>')
        s.append(f'<text x="{xx:.1f}" y="{mt+ph+20}" text-anchor="middle" font-size="11">{b}</text>')
    s.append(f'<text x="{ml+pw/2}" y="{Hd-12}" text-anchor="middle" font-size="13">minimum required bit depth b</text>')
    s.append(f'<text x="20" y="{mt+ph/2}" font-size="13" transform="rotate(-90 20 {mt+ph/2})" '
             f'text-anchor="middle">relative bandwidth (b/8)</text>')
    lx = ml + pw + 16
    s.append(f'<text x="{lx}" y="{mt+4}" font-size="12" font-weight="bold">scene / curve → min b</text>')
    for k, ((tier, curve), b) in enumerate(sorted(mins.items())):
        xx, yy = X(b), Y(b / 8.0)
        color = PALETTE[tier]
        extra = ' fill-opacity="0.5"' if curve == "aces" else ""
        s.append(f'<circle cx="{xx:.1f}" cy="{yy:.1f}" r="5" fill="{color}"{extra}/>')
        s.append(f'<text x="{lx}" y="{mt+26+k*20}" font-size="11.5"><tspan fill="{color}">●</tspan> '
                 f'{tier}/{curve}: {b}-bit, {b/8:.2f}x bandwidth</text>')
    # 8-bit reference
    xx = X(8)
    s.append(f'<line x1="{xx:.1f}" y1="{mt}" x2="{xx:.1f}" y2="{mt+ph}" stroke="#888" stroke-dasharray="4 4"/>')
    s.append(f'<text x="{xx+5:.1f}" y="{mt+14}" font-size="11" fill="#555">8-bit (fails mid/bright)</text>')
    s.append("</svg>")
    open(path, "w").write("\n".join(s))


chart("reinhard", os.path.join(OUT, "tradeoff_reinhard.svg"))
chart("aces", os.path.join(OUT, "tradeoff_aces.svg"))
bandwidth_chart(os.path.join(OUT, "bandwidth_tradeoff.svg"))
print("wrote", *os.listdir(OUT))
