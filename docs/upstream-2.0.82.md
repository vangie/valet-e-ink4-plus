# Metalio 2.0.82 同步记录

- 官方源码：`fd3fa033a61e52dea6326e80724b218d00979767`
- 源码版本：`2.0.82`
- 官方发布目录当前仍显示：`2.0.81`
- 同步策略：完整合并官方变更，再叠加 Valet 自建助理、阅读上下文和 Live 功能

本轮已合入应用商店 ELF 能力扩展、录音与 WAV/Opus 播放、SD 卡初始化与诊断、电源策略、VCOM 查询与黑色深度回退、阅读方向与翻页设置（包括晃动翻页）、壁纸刷新、待机恢复、网络信号/ICCID 显示，以及官方测试页的关机、重启、UI 和 Wi-Fi 入口。Valet 保留国内/国际版官方入口，同时额外保留「自建」助理入口。

模拟器的原版 Tab 和新版 Tab 已按公开源码复核 2.0.81 的可见交互；硬件时序、屏幕刷新质量、SD 卡兼容性和 VCOM 实测仍需设备验收，模拟器不替代真机验证。

验证：

- `python3 tools/upstream/validate.py`
- `python3 -m unittest discover -s tools/tests -p 'test_upstream_sync.py'`
- `NODE_PATH=/tmp/valet-browser/node_modules VALET_CHROME=... node tools/tests/sim_upstream.cjs`
- `IDF_PATH=~/esp/valet-idf-v6.1 idf.py -B build-valet build`

构建结果：`Metalio-E-Ink4-Plus.bin` 为 `0x4f9630` 字节，最小应用分区剩余 `0x69d0` 字节。该版本尚未刷机、部署或推送远端。
