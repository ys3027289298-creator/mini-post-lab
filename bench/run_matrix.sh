#!/usr/bin/env bash
# 三档分辨率 × 三种模式，交错跑 5 轮以均摊系统漂移，输出原始行
set -u
cd "$(dirname "$0")"
FRAMES=300
for rep in 1 2 3 4 5; do
  for res in "1280 720" "1920 1080" "2560 1440"; do
    set -- $res
    for m in sep mrt split; do
      BENCH_SYNC=spin ./bench run "$m" "$1" "$2" "$FRAMES"
    done
  done
done
