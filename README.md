# mini-post-lab

后处理管线基础模块，C 实现，每目录独立构建可自测。

| 目录 | 内容 |
|---|---|
| `tonemap/` | Reinhard 与 ACES 曲线、曝光、亮度直方图 |
| `bloom/` | 亮度阈值提取、可分离盒式模糊 |
| `dof/` | 弥散圆计算、模糊半径与钳制 |
| `lut/` | 3D LUT 三线性插值、均匀/幂律非均匀格点；`lut/ANALYSIS.md` 含误差界与感知格点设计推导，`make bench` 出取舍曲线 |
