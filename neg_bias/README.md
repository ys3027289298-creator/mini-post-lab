# neg_bias

色调映射前三步减法（高光阈值相减、锐化邻域相减、色差通道相减）的负值产生分析与
偏置取舍脚手架。纯 C、固定随机种子、三档 1080p 合成 HDR 场景。

- `make run`：输出逐档逐步的负值统计、达标最小偏置 b\*、半量化级预算交叉点，
  以及有符号/钳零两条管线的端到端 RMSE；扫描曲线写入 `out/*.csv`
- `python3 -I tools/gen_data.py`：把 CSV 与统计结果生成 `report/data.js`
- `report/bias_curves.html`：负值占比与亮度抬升两条曲线（含达标线/预算线、tooltip、数据表）
- `report/report.md`：完整推导与结论
- `node tools/smoke_chart.cjs "$PWD/report"`：图表交互冒烟测试

真实像素 dump 接入方式：用 dump 数据替换 `build_scene()` 的输出数组，其余分析代码不变。
