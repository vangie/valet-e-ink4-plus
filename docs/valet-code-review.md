# 固件代码审查与空间优化（2026-11-19）

本轮重点审查新增自建助理协议、阅读上下文会话、音频队列、配置保存和共享 WebSocket 音频解析。保留原工作区的全部开发改动。共享网关继续固定正式 v0.2.0 / d935cf3，未改动 Passport、已发布安装包或生产服务。

## 已修复

- **P1，音频越界读取**：原 WebSocket v2/v3 直接转换未对齐、只读缓冲并相信 payload_size。现在先检查完整头部、实际负载长度，按字节读取网络序，拒绝截断、夸大长度和多余尾部；JSON 按实际消息长度解析。
- **P1，打断后旧音频重新入队**：解码在队列锁外执行，清队列后仍会推回旧 PCM。为播放队列增加 generation，打断、停止、flush 均使旧结果及等待入队者失效。已交给 I2S 的当前短帧不能靠清软件队列撤回，需实机确认打断延迟。
- **P1，配置覆盖**：配对更新 token 与设置/网页修改可能基于不同旧快照。持锁完成 NVS 保存和内存更新，用仅存在内存的 revision 拒绝旧快照；设备设置排队时只携带操作/id，在存储任务执行时重读最新配置。
- **P1，多助理保存上限错误**：NVS string 最多约4000 bytes，原配置模型允许8192 bytes。改为同键 blob 并兼容旧 string 读取。仍受16 KiB NVS总体容量约束；空间不足返回失败，内存配置不提前宣布成功。旧固件没有 blob 读取能力，回退旧开发固件前应另行导出配置。
- **P2，重复播报和旧轮确认**：每轮 Ask 使用独立 request ID，speech 确认也带该轮 ID；完成事件去重，开始新录音时取消旧问答/播报并清除其run关联；忽略旧轮迟到确认/完成/错误。
- **P2，Ask/Live模式切换串扰**：连接打开时冻结会话模式，旧协议实例不再读取界面新选择来决定如何处理迟到消息。
- **P2，收尾和播放背压**：先等待 receiver 结束再清理其可写状态；音频入队最多等待500ms。自建音频回调返回入队结果，拥塞会清播放并报告错误，避免静默丢音和错误确认已播放。每个上传分片检查closing。网络写入/驱动本身的阻塞时间仍需设备断网测试。
- **P2，传输并发**：WebSocket发送锁覆盖分片状态和帧构造，而非仅最后一次TCP写入。
- **P2，设置读取**：NVS字符串第二次读取失败时返回默认值，避免使用读取失败后的缓冲。

## 空间优化

FreeType 原先整个库覆盖为-O2。现在格式解析等使用-Os，TrueType解释器、smooth/raster栅格化保留-O2和跳转表。编译命令已逐项确认，未删字体格式、字库、BLE、官方功能、资源或调整分区。尚未测设备翻页耗时，解析阶段性能需实机比较。

长录音按值移动至WAV容器，预留58-byte头部空间，随后释放WAV再生成完整JSON；cJSON附件引用现存Base64字符串，发送期间保证其生命周期。30秒16kHz mu-law录音的两项副本约为480058-byte WAV及640080-byte Base64，总计约1.07 MiB潜在峰值分配被去除；这不是设备实测heap峰值。主机测试验证480000-byte录音封装不重新分配，输出及真实Adapter互操作不变。

最终镜像尺寸及RAM数据见下方构建结果。分区余量仍较紧，不建议据此直接判断可继续无限增加功能。

## 验证及边界

- ESP-IDF v6.1 / ESP32-S31完整构建与分区尺寸检查。
- ASan/UBSan：配置编解码及真实配置缓存/保存代码（存储边界为fake）、旧string迁移读取、超过4KiB blob、失败提交、同时写入、过期快照不覆盖配对token。
- ASan/UBSan：实际音频帧解析函数，v1/v2/v3、所有截断边界、夸大长度、只读缓冲。
- 阅读上下文快照、书本/助理/独立会话隔离与UTF-8截断测试。
- 配对解析及C++签名/WAV与真实OpenClaw代理、Hermes Adapter互操作；30秒WAV移动复用分配测试。
- 共享网关Node plugin/relay/IMA/context/media回归全部通过；Python40项中35通过、5项依赖真实Hermes宿主的测试本机跳过。

未刷机。本轮未以真机验证音频线程调度、回声、打断尾音、NVS断电迁移和长时4G断网重连；主机测试和构建不能替代这些验收。共享网关仅复核已有实现并运行回归，没有在本轮升级或发布新版本。

复现：

```sh
sh tools/tests/run_assistant_profiles_test.sh
sh tools/tests/run_websocket_audio_test.sh
sh tools/tests/run_valet_reading_context_test.sh
IDF_PATH=/path/to/esp-idf sh tools/tests/run_valet_pairing_test.sh
IDF_PATH=/path/to/esp-idf VALET_TEST_PYTHON=/path/to/python \
  PASSPORT_SOURCE=/path/to/ai-passport-source sh tools/tests/run_valet_wire_test.sh
# 在已激活的ESP-IDF v6.1环境中：
idf.py -B build-valet build
```

## 最终构建结果

| 指标 | 审查前 | 审查后 |
| --- | ---: | ---: |
| app.bin | 5,223,920 bytes | 5,169,824 bytes |
| 5 MiB应用分区余量 | 18,960 bytes（18.52 KiB） | 73,056 bytes（71.34 KiB） |
| FreeType链接贡献 | 420,195 bytes | 365,293 bytes |

所有审查修复计入后，镜像净减少 **54,096 bytes（52.83 KiB）**。链接报告DIRAM静态贡献增加12 bytes，外部RAM静态贡献不变；动态音频/模型内存需要实机测量。

最终app SHA256：`534d3c891f215dedd2275e68ccd88b8b9bceeee439a432f3e721aaf7ce9c85e0`。未刷写设备。
