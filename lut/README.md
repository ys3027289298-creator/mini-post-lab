# lut

3D 颜色 LUT：幂律格点 `x_i = (i/(N-1))^p`（p=1 即均匀）+ 8 角点三线性插值。

- `make test` 跑自检：格点精确性、Δ/4 插值界、三档亮度感知误差（JND=2.3）、
  均匀 FMA 快路径与通用二分路径的耗时对比。
- `make bench` 扫描 p=1.0–4.0，输出 `analysis/sweep.csv`。
- `python3 -I analysis/analyze.py` 复核解析公式并生成
  `analysis/report.html`（感知误差/格间距/查表开销三条取舍曲线）。
- 推导全文见 `ANALYSIS.md`。
