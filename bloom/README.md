# bloom

`make test` 跑自检。

分辨率一致性标定：

```sh
python3 tools/resolution_calibration.py
python3 tools/tradeoff_chart.py
```

脚本会在 640×360、1280×720、1920×1080 三档分辨率上运行同一个径向灯场景，
输出 `artifacts/bloom_resolution_results.{json,csv}` 与
`artifacts/bloom_tradeoff.png`。
