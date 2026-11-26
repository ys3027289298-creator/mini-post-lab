# bloom

`make test` 跑自检。

- `src/bloom.c`：亮度阈值提取（bright-pass）与可分离盒式模糊的基础自检。
- `src/bloomcast.c`：三通道泛光衰减、按亮度加权核宽导致的色偏、JND 反解核宽
  上限，以及三档光源强度实测。推导与实测解读见 [CAST.md](CAST.md)，曲线数据
  与图在 `../artifacts/`（`bloomcast_curves.csv`、`bloomcast_curves.png`）。
- `tools/plot_curves.py`：从自检输出的 CSV 画色偏/高光层次两条决策曲线。
