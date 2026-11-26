# bench — 后处理输出策略代价台

复现「单独缓冲 sep → 多目标 mrt」的性能回归，并验证修复版 split。

## 代价模型

像素数 N，每次通道切换固定开销 c_fix，每字节·像素带宽时间 t_b，每像素计算 t_alu：

| 策略 | 通道数 P | 每像素字节 B（读 22 + 写） | 总耗时 |
|---|---|---|---|
| sep（上一版） | 3 | 36 = 22 + (4+2+8) 原生格式 | 3c_fix + N·36·t_b + N·t_alu |
| mrt（这一版） | 1 | 22 + 3s，s 为统一格式字节 | c_fix + N·(22+3s)·t_b + N·t_alu |
| split（修复） | 2 | 34 = 22 + (4+8)，CoC 打包进 RGBA8 alpha | 2c_fix + N·34·t_b + N·t_alu |

- 固定项 = P·c_fix：线程派发 / GPU 通道切换，与分辨率无关。
- 带宽项 = N·B·t_b：按真实格式字节数分配缓冲并逐字节写，实测流量非估算。
- 计算项三模式相同（ACES + CoC + bright-pass 每帧各一遍），模式差值中相消。

## 回归条件（格式被迫升级）

mrt 要求三目标同分辨率同格式。原生写带宽 14 B/px（RGBA8=4, R16F=2, RGBA16F=8），
统一到 s 后写带宽变 3s。回归当且仅当省下的两次切换抵不过多写的带宽：

```
T_mrt > T_sep  ⇔  3s − 14 > 2·c_fix / (N·t_b)
```

## 反解可接受格式（T_mrt ≤ T_sep）

```
s ≤ (14 + 2·c_fix/(N·t_b)) / 3
```

本机拟合 c_fix=6.53 µs、t_b=0.0266 ns/(B·px)，三档预算 s ≤ 4.71~4.84 B/px：
RGBA8(4B) 在预算内但装不下 HDR bloom（内容非法），RGBA16F(8B) 三档全部超预算。
临界像素数 N* = 2c_fix / (10 t_b) ≈ 0.05M px，三档分辨率都是它的 19~75 倍，
即带宽项主导，省切换必然亏。

## 被迫升级的目标

| 目标 | 原生 | mrt 统一 RGBA16F | 1440p 整帧多写 |
|---|---|---|---|
| color（tonemap 输出） | RGBA8 4B | 8B，+4B/px | 14.75 MB |
| coc（dof 输出） | R16F 2B | 8B，+6B/px | 22.12 MB |
| bloom | RGBA16F 8B | 8B，无升级 | 0 |

## 实测（spin 屏障，每配置 8 轮取 min，500 帧/轮）

| 分辨率 | sep ms | mrt ms | split ms | mrt 回归 | split 回归 |
|---|---|---|---|---|---|
| 1280x720 | 2.927 | 2.981 | 2.335 | +1.9% | −20.2% |
| 1920x1080 | 5.867 | 6.213 | 5.022 | +5.9% | −14.4% |
| 2560x1440 | 10.110 | 10.389 | 9.127 | +2.8% | −9.7% |

三档 mrt 均未回到上一版水平；split 三档全部回到上一版水平之下。

## 复现实验

```sh
make
./bench calibrate                 # pthread 屏障 c_fix
BENCH_SYNC=spin ./bench calibrate # 自旋屏障 c_fix（模拟 GPU 调度粒度）
./run_matrix.sh > results_final.txt
python3 -I aggregate.py results_final.txt
python3 -I model.py results_final.txt
python3 make_figs.py              # 需用户 site 的 matplotlib，故不加 -I
```

`BENCH_SYNC=pthread`（默认）切换开销 ~440 µs/次，固定项主导，mrt 虚假占优；
`BENCH_SYNC=spin` 切换 ~6.5 µs/次，贴近 GPU 上带宽主导的真实比例，回归才显现。
WSL2 下自旋线程对抢占敏感，故用交错轮次 + min-of-N 统计。
