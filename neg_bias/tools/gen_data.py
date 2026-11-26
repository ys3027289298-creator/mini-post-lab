#!/usr/bin/env python3 -I
"""Emit chart data (sweep curves + headline stats) from the C harness output."""
import csv
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out")

scenes = ["dark", "mid", "bright"]
stages = [("bloom", "bright_pass"), ("sharpen", "unsharp"), ("ca", "ca")]

data = {"scenes": scenes, "curves": {}, "stats": []}
for sc in scenes:
    data["curves"][sc] = {}
    for slug, _ in stages:
        pts = []
        with open(os.path.join(OUT, f"{sc}_{slug}.csv")) as f:
            for row in csv.DictReader(f):
                pts.append([float(row["bias"]),
                            float(row["neg_ratio"]),
                            float(row["tonemapped_lift"])])
        data["curves"][sc][slug] = pts

with open(os.path.join(OUT, "results.txt")) as f:
    for line in f:
        line = line.strip()
        m = re.match(r"(dark|mid|bright),(bright_pass|unsharp|ca),(.*)", line)
        if not m:
            continue
        sc, stage, rest = m.groups()
        rec = {"scene": sc, "stage": stage}
        for kv in rest.split(","):
            k, v = kv.split("=")
            rec[k] = float(v) if k not in ("neg", "pass", "neg_at_budget") else int(v)
        data["stats"].append(rec)

with open(os.path.join(ROOT, "report", "data.js"), "w") as f:
    f.write("window.NEG_BIAS_DATA = ")
    json.dump(data, f, indent=1)
    f.write(";\n")
print("wrote report/data.js",
      sum(len(v) for sc in data["curves"].values() for v in sc.values()),
      "curve points,", len(data["stats"]), "stat rows")
