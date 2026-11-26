#!/usr/bin/env python3
"""聚合 bench 原始输出：按 (分辨率, 模式) 统计每帧耗时的 min/median，算相对 sep。"""
import re
import statistics
import sys
from collections import defaultdict

PAT = re.compile(
    r"sync=(\w+) mode=(\w+) res=(\d+)x(\d+) frames=(\d+) passes=(\d+) "
    r"bytes_per_px=(\d+)\(read=(\d+)\+write=(\d+)\) per_frame_ms=([\d.]+)"
)

rows = defaultdict(list)
for line in open(sys.argv[1]):
    m = PAT.search(line)
    if not m:
        continue
    sync, mode, w, h, frames, passes, bpp, rd, wr, ms = m.groups()
    rows[(int(w), int(h), mode)].append(
        dict(passes=int(passes), bpp=int(bpp), rd=int(rd), wr=int(wr),
             ms=float(ms)))

base_min, base_med = {}, {}
for (w, h, mode), runs in rows.items():
    vals = [r["ms"] for r in runs]
    if mode == "sep":
        base_min[(w, h)] = min(vals)
        base_med[(w, h)] = statistics.median(vals)

print(f"{'res':>9} {'mode':>5} {'passes':>6} {'B/px(r+w)':>11} "
      f"{'min_ms':>9} {'med_ms':>9} {'vs_sep(min)':>12}")
for (w, h, mode), runs in sorted(rows.items()):
    vals = [r["ms"] for r in runs]
    r0 = runs[0]
    mn, md = min(vals), statistics.median(vals)
    delta = mn / base_min[(w, h)] - 1.0
    flag = "  <-- 回归" if delta > 0.05 else ""
    print(f"{w}x{h:>5} {mode:>5} {r0['passes']:>6} "
          f"{r0['bpp']:>4}({r0['rd']}+{r0['wr']:<2}) {mn:>9.4f} {md:>9.4f} "
          f"{delta*100:>+11.1f}%{flag}")
