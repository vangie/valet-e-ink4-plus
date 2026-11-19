# Valet 随行 · 固件实现进度

模拟器 `sim/` 是产品交互参考。下表描述真实固件的实现边界，不把模拟器状态当成设备能力。

| 模块 | 当前固件 |
| --- | --- |
| 首页 | 新增首页第一屏：提问、通话、当前助手、继续阅读、待办。应用列表保留原渲染器与主题，灰键切页。 |
| 设置 | 六组入口下钻到现有功能页。总览提供前光和音量；灰键分页滚动，菜单逐级返回，长按菜单回首页。 |
| 自建助理 | 最多八个本地配置、当前选择、单连接旧配置导入。保存于一条 NVS 记录，UI 只读取内存副本，写入走 Storage 任务。 |
| 网页配对 | 已接入 STA 局域网配置服务（8080）。设备添加入口显示真实二维码，手机页提供提示、配对 JSON 解析、添加/选择/移除。配置保存后首次进入提问/通话完成签名握手；可能需要网关批准新设备。 |
| 自建问答与 Live | 已接 Passport v1 配对、问答与IMA Live；新版增加带书Live和按答案run绑定的语音播报，可单独关闭播报。尚未E-Ink4实机验收。 |
| 书本上下文 | 已实现可见页快照、按书会话、独立问答切换、按助理/书保存分离的 RAM 历史，以及网关协商/正文发送。需配套开发分支网关；尚未真机验收。 |

保存连接与在线状态分开。导入连接不自动获得 ask/live/context 能力。首次签名配对成功可确认认证和 Ask 协议；Live 需实际创建并收到 ready，书本上下文尚未启用。

配置列表 JSON 有版本号、唯一 ID、活动 ID 校验；损坏数据不会部分导入。总记录限制为 8 KiB，最多八个连接，共享官方 16 KiB NVS。配对数据不写入日志。

## 本机编译

ESP-IDF v6.1 安装于 `~/esp/valet-idf-v6.1`，不覆盖原有 5.5 环境。资源生成还需要 IDF Python 环境中的 `Pillow` 和 `qoi-conv`。

```sh
. ~/esp/valet-idf-v6.1/export.sh
idf.py -B build-valet build
```

应用镜像位于 `build-valet/Metalio-E-Ink4-Plus.bin`。首次构建通过；5 MiB 应用分区仅剩约 28 KiB（以最新构建日志为准），添加网关协议和本地网页前需要重新测量、精简资源或设计升级方案。当前没有修改分区表。

仅编译并不验证墨水屏排版、触控、保网和待机生命周期；这些需真机验收。未执行烧录。

## 多助理模型主机测试（macOS / Homebrew cJSON）

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main -I /opt/homebrew/include/cjson \
  main/assistant_profiles_model.cc tools/tests/assistant_profiles_test.cc \
  -L /opt/homebrew/lib -lcjson -o /tmp/valet-assistant-profiles-test
/tmp/valet-assistant-profiles-test
```

测试覆盖保存格式读回、多连接切换、删除活动项、空列表、重复 ID、损坏数据与容量上限。设备 NVS 故障和 UI 生命周期仍需单独验证。

## 本地配置页

设置 → 助手 → 自建 → 添加助理。当前批次要求设备已经连接 Wi-Fi，手机或电脑在同一局域网。服务使用 8080 端口，避免与官方 80 端口文件服务抢占；未开启临时热点，也未实现 mDNS。

设备二维码包含 `http://<STA IP>:8080/setup#<一次性访问码>`。地址和访问码也可手输。访问码通过请求头校验，不进入查询串；关闭配置、待机、离开设置或十分钟超时后失效。设备保网与禁止自动待机仅在配置服务开启期间持有，关闭后释放。配对码和长期凭据不会通过列表 API 回读。

网页以 gzip 嵌入固件，无 CDN 或外部脚本依赖。普通 HTTP 下复制功能自动回退到选中全文手动复制。提示词引用现有 Passport 项目，不编造 Valet 插件，明确区分网页保存、签名认证和实机语音测试。

POST 返回任务已接收，网页继续查询设备结果；NVS 写入均走内部 RAM 的 Storage 任务。二维码解析与地址绑定通过真实 Mbed TLS base64 主机测试；Chrome / WebKit 手机测试使用模拟 API 验证网页操作，不能替代 ESP HTTP 服务和设备 NVS 的实机验收。

```sh
IDF_PATH=~/esp/valet-idf-v6.1 sh tools/tests/run_valet_pairing_test.sh
```

## 共享网关与联调（2026-11-18）

共享开源项目：https://github.com/vangie/valet-gateway 。本仓库以 `integrations/valet-gateway` submodule 固定版本；独立升级插件，不随固件静默重启服务。保留 `ai-passport-*` 插件 ID、`/passport/v1`、Passport 身份和配对存储，增加签名设备型号 `esp32s31 / e-ink4-plus`。Passport 仓库未迁移，现有部署未替换。

- Kyan / OpenClaw 2026.11.4、Passport 插件 0.7.3：临时副本和 18791 端口转发到真实网关。C++ 产生签名完成首次配对、device token 重连、会话订阅、Live 创建/ready/close。测试提问被接受，但未在 20/65 秒内收到最终回复；定向取消测试 run，不计为问答端到端通过。初始 status 的 upstreamConfigured=false，但实际 Live 创建后能 ready，需以真实 session 验证为准。临时测试设备已移除。
- Elise / Hermes v0.23.1、现有插件 0.7.2：新共享源码在临时目录、独立端口 18792 和临时设备库中运行；复用主机的实际实时模型配置。签名认证、Live 创建、实际提供方 ready、IMA appendAudio、close 通过。没有替换或重启现有 Hermes 服务。
- 共享插件回归：OpenClaw plugin/relay/IMA 测试通过；Hermes 原有 30 项测试在 Elise 真正运行环境通过。本地新增 E-Ink4 身份测试通过，宿主适配测试本地会跳过四项。

尚未验证：设备物理麦克风/扬声器、4G 吞吐、回声抑制、长时通话、真实语音提问完整响应。当前使用手动打断；不宣称已具备 Passport 的语音回声判别打断。阅读上下文和独立书本会话仍待协议扩展，不发送正文到通用会话冒充完成。


联调对 Passport 的影响：所有远程修改仅在 `/tmp/valet-*` 隔离副本和独立端口。没有覆盖已安装插件、重启网关、修改现有设备 token；Kyan 的临时测试设备已移除。提问联调只定向取消测试 run。独立仓库保留旧插件 ID 和持久化格式，后续可分别安排部署，不要求双设备同时升级。

可复现 C++ 签名与真实 Adapter 互操作测试：

```sh
IDF_PATH=~/esp/valet-idf-v6.1 \
PASSPORT_SOURCE=/path/to/existing/ai-passport-agent \
VALET_TEST_PYTHON=/path/to/python-with-aiohttp-and-cryptography \
sh tools/tests/run_valet_wire_test.sh
```

该测试不修改现有网关，不向外部模型发送请求；在临时目录验证真实 Python/JS Adapter 与 C++ 消息格式。

## 共享网关正式版本固定

E-Ink4 已将 submodule 从 `95885ce` 显式更新到 Valet Gateway v0.1.0，提交 `40cbd826504bbf65a26f50bcea604ab5a0335351`。对比确认没有插件运行逻辑变化，新增发布工具、校验清单和安装文档；安装包仍编号 0.7.5。设备本地配置页及模拟器提示词附固定版本和正式 Release 地址。本步骤没有部署或重启现有网关。

Passport 会话补充的实物联调结果：其设备已取得真实 Ask final，Kyan 主模型 502 后 fallback 导致等待较长。此结果解释此前网关联调超时，但不替代 E-Ink4 设备端问答与音频验收。

## 当前页提问与会话隔离

共享网关功能分支 `feature/book-context-v1`，固定提交 `73939cd`（不是正式 v0.1.0）。v0.1.0 tag 与 Passport 依赖没有变化。本次没有部署或重启生产网关。

- 正文渲染后发布快照：文件路径的稳定 128-bit 哈希、书名、章节、1-based 页码和当前页文字。数据只来自渲染页面，不在 UI 里重新加载整本书；纯图片页不伪造文字。正文最多 12000 UTF-8 bytes，截断时显示节选。
- 从阅读长按 AI 进入时选择这本书，从首页提问/通话进入时清除书本上下文。助手页顶部显示书名/章节/页码，点击切换随口问；独立问答可点击带上最近已显示的一页。
- 同一本书翻页后继续同一远端会话；不同书文件、不同助理不共用显示历史。本地最近最多六场历史（每场最多600个flow项）保存在本次启动的RAM，不声称跨重启持久化。后端历史独立留存；正文作为用户引用资料，不提升工具权限。
- 固件先协商 `valet-book-context-v1`，等待 `valet.context.set` 的确认，再订阅该session，成功后才允许录音提交。旧网关会明确提示升级，不能把书的问题静默发到随口问。
- Hermes 以 native thread_id 按书隔离，语音转写后组合正文与问题；history/transcript/abort也限定session。OpenClaw在代理中检查session归属并将正文作为用户引用内容前置到message，音频附件格式保持原样。
- 书本Live暂不支持（`valetBookLive:false`），独立Live不变。

验证：C++主机快照测试覆盖选中快照不可变、同书续聊、同名不同文件隔离、助理隔离、UTF8截断与空页；Node新上下文/旧Passport测试通过。Elise真实Hermes运行时隔离目录共37项全部通过，含真实Adapter WebSocket的语音正文组合、原生session key差异、独立会话不带正文、历史/转写隔离。语音转写和模型回复在该测试中是桩，未宣称真实模型已阅读书页。ESP32-S31完整编译通过，需设备验收顶部条、返回阅读位置与实际采音。

```sh
sh tools/tests/run_valet_reading_context_test.sh
```

## v0.2.0：带书 Live 与问答播报

在书本提问页点「通话」保留当前页与书本session；通话中「返回提问」回到同一会话。网关将选中页面作为引用资料加入实时模型指令，并在工具咨询时继续传递同书上下文。旧Passport未声明能力时不走这些新增路径。

Ask final在显示文字后可自动请求 `valet.speech.start`，只传sessionKey/runId，网关读取有授权的已完成答案进行合成，不接受客户端任意播报文本。`valet-ask-speech-v1` / `valetAskSpeech:1`独立协商。播报通过24kHz IMA音频流入设备原播放队列；取消、切换场景或新问题抑制旧音频。设备设置的播报开关保存为独立 `agent_tts`，不改官方云偏好。播报错误保留文字。

本轮选择插件内OpenAI语音（默认gpt-4o-mini-tts/coral），复用已有私有文件或环境变量，不修改Hermes全局xAI设置。Elise默认xAI TTS缺少凭据；隔离测试明确改为插件speech.provider=openai，复用现有realtime私有凭据文件后真实合成285600 bytes PCM（75帧）；Kyan复用已有OpenAI私有文件真实合成165600 bytes PCM（44帧）。仅隔离测试，没有改生产网关配置。

真实带书Live：Elise临时服务与数据库完成book context选择、实时提供方ready、IMA上传与正常close；Kyan新Provider在临时目录保留gpt-realtime-2.1模型配置完成book桥ready。40项Python测试在真实Hermes运行环境全通过；Node旧Passport与新上下文/播报/限速/取消/模型保留测试通过。以上不替代设备物理播放、麦克风和4G验收。

正式版：Valet Gateway v0.2.0 / 插件0.8.0，提交 `d935cf3c90c51fd783d9d327cbfc86457771c83f`。E-Ink4 submodule已固定该正式提交，旧v0.1.0及Passport依赖不变。Release含独立安装包、完整源码、manifest和SHA256SUMS。Node22/24/26 CI通过，未替换现有生产网关。


## 代码审查与空间优化

本轮修复、尺寸对比、回归与待实机验证项目见 [valet-code-review.md](valet-code-review.md)。共享网关仍固定v0.2.0；没有刷机或修改生产服务。

## 官方云服务（百问、待办、天气、推送、应用商店、WorkBuddy）

公开源码的 `main/cloudzao_endpoints.c` 把服务地址全部留空。官方烧录平台上的整片镜像是完整构建，地址都在里面。本地生成一份填好地址的副本：

```sh
python3 tools/official/extract_endpoints.py <官方整片镜像.bin>
```

脚本写出 `main/cloudzao_endpoints.local.c`（已 git-ignore，不提交），构建时优先用它，配置日志会打印 `Cloud endpoints: cloudzao_endpoints.local.c`。只填国内版；国际版分支保持空白。官方构建本身也空着的 7 项保持空白。布局变了脚本会报错退出，不会写错地址。

这只解决地址。百问的账号由服务器按设备身份（MAC、NVS 里的 UUID、eFuse 序列号与 HMAC）识别，所以刷机时只写应用分区，不要擦 NVS。官方订阅是否允许第三方固件使用，需以官方说明为准。

## 桌面测试构建与开发链路

`tools/dev/build.sh` 把测试固件编到 `build-devtest/`：源码和 `sdkconfig` 与正式构建相同，再叠加 `sdkconfig.devtest`（关闭触摸芯片自动升级，打开 `CONFIG_VALET_DEV_LINK`）。仓库里的 `sdkconfig` 不会被改动。

`CONFIG_VALET_DEV_LINK` 在 USB 串口上增加 `@DEV` 命令：读出界面画好的那一帧、注入触摸和按键、报告当前页面。拿到 USB 线的人就能操作设备，所以正式构建保持关闭。电脑端用 `tools/dev/valetdev.py`（需要 pyserial，IDF 的 Python 环境里有）：

```sh
tools/dev/valetdev.py serve &        # 占住串口并记日志，其他命令都经过它
tools/dev/valetdev.py shot home.png  # 截图，竖屏 684×1216
tools/dev/valetdev.py tap 342 600    # 坐标与截图像素一致
tools/dev/valetdev.py key home       # home / prev / next / ai / vol_up / vol_down / power
tools/dev/valetdev.py flash          # 只写 ota_0 应用槽，写完重新连上
```

- 截图是固件要求面板显示的内容；墨水屏上的灰度、残影和局部刷新效果不在里面，仍要看真机。
- 注入的触摸从触摸驱动的采样入口进入，之后与真手指走同一条路径（盖板键识别、震动、短按回放）；但它绕过了触摸芯片，芯片上报坐标的问题测不出来。
- 这台设备的串口在 RTS=1、DTR=0 时会复位芯片，macOS 打开串口时默认两根线都拉高再各自放下，所以普通的打开方式会让设备重启。`valetdev.py` 先放 RTS、后放 DTR，打开时不复位。
- 复位后约 2 秒设备会在 USB 上重新枚举一次，在那之前打开的串口会失效；`flash` 会等过这一段再连。设备进待机或关机后串口同样失效，需要手动 `attach`。
