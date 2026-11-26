# color_audit

色彩管理事故的复现、量化与方案模块：同一后处理（加权求和/盒式模糊）在线性空间
与伽马空间执行时的跨机差异审计。

| 路径 | 内容 |
|---|---|
| `src/color_audit.c` | 参考核：线性/伽马两条混合与盒式模糊路径、双机器模型，含自测与 CLI |
| `tools/audit.py` | 32×32 测试帧、三档伽马 × 两台机器逐像素码差审计，超 1 量化级即 FAIL |
| `tools/make_curves.py` | 三张决策图：混合差异、伽马容忍带反解、一致性/开销取舍 |
| `results/` | 审计 JSON 与图表（生成物） |
| `REPORT.md` | 完整推导与结论 |

## 构建与运行

```bash
make test                       # C 核自测，all_ok=1
./color_audit 1.8 2.4 gam       # 单像素双管线对照
python3 -I tools/audit.py       # 像素审计，结果写 results/audit_report.json
python3 -I tools/make_curves.py # 曲线输出到 results/
```

## 判定规则

两台机器的输出统一按参考伽马 2.2 编码为 8-bit 码值，逐像素逐通道比较；
任一通道码差 > 1（即 1/255）判 FAIL，并记录像素坐标与双侧伽马设置。

## 关键结果

- 线性管线：三对机器全部 PASS（最大码差 0.00）
- 伽马空间管线：全部 FAIL，最差 26.4 个量化级（γ 1.8 vs 2.4）
- 伽马可接受偏离带：相对 ±1.07%（γ=2.2 时绝对带 [2.176, 2.224]）
