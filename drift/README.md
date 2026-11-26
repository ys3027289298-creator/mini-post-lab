# drift

累积偏移判型与清零周期脚手架。60 fps 帧等价执行 1 / 10 / 60 分钟三档，输出线性固定偏差与平方根舍入随机游走两组对照。

```sh
make data
```

产物：

- `out/runs.csv`：三档裸跑偏移与 10 秒清零策略结果；
- `out/reset_curves.csv`：清零间隔—漂移包络—连续性曲线；
- `out/continuity_check.csv`：周期性重启的扩展累计平均的帧间连续性实测；
- `report/report.md`：公式、反解值、判定与清零策略；
- `report/drift_curves.html`：交互图表。

主口径为 `q=1/256`、半量化级预算 `1/512`。
