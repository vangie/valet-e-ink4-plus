#pragma once
#include "protocol.h"
#include "sdkconfig.h"
#include "assistant_profiles.h"
#include "valet_reading_context.h"
#include "opus_decoder.h"
#include "opus_resampler.h"
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <atomic>
#include <mutex>
#include <deque>

class ValetProtocol : public Protocol {
public:
    ValetProtocol();
    ~ValetProtocol() override;
    static bool InitializeIdentity(); // startup internal-RAM task
    static void SetLive(bool enabled);
    static bool Live();
    static void SetMuted(bool enabled);
    static bool Muted();
    // 通话里麦克风最近一包的声音大小（均方根，满幅 32767）和时刻；通话页的波形用。人声门关着时为 0。
    static int MicLevel(int64_t* at_us = nullptr);
    // 通话中线路断了、正在自动接回（通话页显示「重新连接」）。
    static bool Reconnecting();
#if CONFIG_VALET_DEV_LINK
    static void DropLinkForTest();   // 仅测试版：主动关掉当前连接，模拟线路断开
#endif
    // 配对结果。Refused* 是网关给出的明确原因，Unreachable 是没连到网关。
    enum class Pairing { Paired, NeedsApproval, CodeExpired, ScopeDenied, Refused, Unreachable, SaveFailed, NoProfile };
    // 用配对码登录一次并保存长期凭据；已有长期凭据时直接返回 Paired。Network 任务调用，阻塞。
    static Pairing PairNow(const std::string& profile_id);
    static const char* PairingText(Pairing result);
    bool Start() override { return true; }
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string&) override {}
private:
    bool SendText(const std::string&) override;
    bool Send(cJSON* message);
    bool Connect(bool bootstrap);
    void Receive(const char* data, size_t length);
    void Handle(cJSON* root);
    void Event(const char* type, const char* state, const std::string& text = {});
    bool Submit();
    void CreateCall();
    std::unique_ptr<WebSocket> socket_;
    EventGroupHandle_t events_;
    valet::AssistantProfile profile_;
    valet::ReadingContext context_;
    bool context_supported_ = false;
    bool context_selected_ = false;
    bool book_live_supported_ = false, speech_supported_ = false, speech_active_ = false;
    uint64_t speech_sequence_ = 0;
    void CompleteReply(const std::string& text);
    std::atomic<bool> authenticated_{false}, opened_{false}, closing_{false}, recording_{false}, live_ready_{false}, interrupted_{false};
    std::mutex inbox_mutex_;
    std::mutex send_mutex_;
    std::recursive_mutex state_mutex_;
    std::deque<std::string> inbox_;
    TaskHandle_t receiver_ = nullptr;
    bool reached_ = false;   // 上一次 Connect 是否连到了网关（WebSocket 已建立）
    std::string refusal_;    // 网关拒绝登录时给的代码（error.details.code），没有则为空
    Pairing Pair();          // 用 profile_ 里的配对码登录并保存长期凭据
    Pairing Refusal() const;
    bool ConnectOnce(bool bootstrap);
    void RunReceiver();
    void StopReceiver();
    // 通话中断线自动重连：新开一个任务重新登录、订阅、建通话，最多试 90 秒；
    // 期间通话不结束（IsAudioChannelOpened 仍为真，麦克风不上传），接不回来才报错。
    std::atomic<bool> reconnecting_{false}, stop_reconnect_{false};
    TaskHandle_t reconnector_ = nullptr;
    // 单次建连（TCP + TLS + WebSocket 握手）的时限。正常 1.6～3 秒连上（2026-11-22 实测，Wi-Fi 和 4G），
    // 连不上时 frp 入口约 10 秒被重置、4G 直连 Cloudflare 约 18 秒被掐断；8 秒足够区分，用户也不用干等。
    int connect_timeout_ms_ = 8000;
    void StartReconnect();
    // 通话还没就绪就被网关关掉（多半是网关连 OpenAI 实时语音一时失败）：隔 2 秒重建，最多 3 次。
    bool call_ready_seen_ = false;
    int call_retries_ = 0;
    int64_t recreate_at_ = 0;
    void RunReconnect();
    void JoinReconnect();
    const char* Prepare();   // 登录后：选阅读会话、订阅消息；失败返回给用户看的原因
    std::unique_ptr<OpusDecoderWrapper> input_decoder_;
    OpusResampler input_resampler_;
    std::vector<uint8_t> recording_data_;
    std::string token_, call_id_, early_ready_, run_id_, reply_;
    // 播放确认点：target 是它到达时已交给播放的包数，播到这里才回确认。
    struct Mark { std::string name; uint64_t target; };
    std::deque<Mark> marks_;
    uint64_t pushed_packets_ = 0, played_base_ = 0;
    int level_peak_ = 0; uint64_t level_sumsq_ = 0, level_samples_ = 0;   // 本段回话收到的声音大小（调音量用）
    // 回话音量自动控制：放大倍数跟着收到的声音走，每一包都保证不超过上限（不破音）。
    float agc_gain_ = 2.0f, agc_env_ = 0.f, agc_gain_min_ = 99.f, agc_gain_max_ = 0.f;
    void ApplyReplyGain(std::vector<int16_t>& pcm);
    int live_errors_ = 0;   // 本次通话里可恢复的实时服务错误次数
    int64_t mic_muted_until_ = 0;   // 播放助理声音时及播完后片刻不上传麦克风（防回声）
    // 人声门：通话里只有检测到有人在说话才上传麦克风，其余时间发静音，免得杂音被当成说话。
    struct Fvad* vad_ = nullptr;
    std::deque<std::vector<int16_t>> gate_preroll_;   // 门关着时最近的几包，开门时一起补上，第一个字不丢
    bool gate_open_ = false, gate_reset_ = true;
    int gate_score_ = 0;
    int64_t gate_last_voice_ = 0;
    float gate_floor_db_ = -60.f;   // 房间底噪（dBFS），慢升快降
    bool GateMic(const std::vector<int16_t>& pcm, int64_t now, bool muted, std::vector<std::vector<int16_t>>& out);
    bool SendLiveFrame(const std::vector<int16_t>& pcm);
    void ResyncPlayback();   // 清空播放队列后：已交出的包都当作播完
    // 缓冲：助理的声音按说话的速度到达，扬声器前面几乎没有存货，网络或屏幕刷新慢一下就断音。
    // 扬声器空着时先攒 kPrerollMs 再一起交出去，之后都直接交；播空了（断过一次）就再攒一次。
    std::deque<std::unique_ptr<AudioStreamPacket>> held_;
    size_t held_samples_ = 0;
    int64_t held_since_ = 0;
    bool holding_ = true;
    void PushReplyAudio(std::unique_ptr<AudioStreamPacket> packet);
    bool HandReplyAudio(std::unique_ptr<AudioStreamPacket> packet);   // 交给播放；失败时清空并报错
    void ReleaseHeldAudio();
    // 通话字幕：助理说话时先攒着，扬声器停下来再上屏（见 Handle 里的说明）。
    struct Caption { bool user; std::string text; };
    std::vector<Caption> captions_;
    // 助理这一段话边说边到的字：网关只在说完时才发整句，被打断时整句不会来，
    // 就用这些字补上（加「……」），屏幕上留着助理说到了哪儿。
    std::string reply_so_far_;
    void KeepInterruptedReply();
    int64_t quiet_since_ = 0;
    int64_t interrupt_at_ = 0;   // 发出打断的时刻，等网关回复用   // 扬声器从什么时候开始没在播（0 = 正在播）
    std::string tool_call_, tool_run_, tool_key_;
    // 助理一句话里可能连着叫两次工具（2026-11-22：「帮我记一下明天八点开会」第一次还没回来就叫了第二次，
    // 旧代码直接结束通话）。一次只跑一个，其余排队；失败或超时只把错误交回给助理，不挂断。
    struct PendingTool { std::string call_id, name, args; };
    std::deque<PendingTool> tool_queue_;
    // openclaw_agent_control（问进度、取消、改主意）：和徽章一样转成 talk.session.steer，很快就有回复，
    // 不占工具槽位，正在跑的 consult 照常跑。请求 id → callId。
    std::deque<std::pair<std::string, std::string>> steering_;
    std::deque<std::string> finished_tools_;   // 最近交回过结果的 callId，网关重发时不再跑一遍
    void StartTool(const std::string& call_id, const std::string& name, const std::string& args);
    void FinishTool(const char* key, const std::string& text);   // 交回当前工具的结果，接着跑下一个
    bool audio_done_ = false;
    bool output_started_ = false;
    bool call_pending_ = false;
    uint64_t playback_sequence_ = 0;
    int64_t ask_deadline_ = 0, last_poll_ = 0, last_ping_ = 0;
    bool awaiting_ = false;
    bool reply_completed_ = false;
    bool session_live_ = false; // Frozen when opening, independent of UI mode changes.
    std::string request_id_;
    int64_t tool_deadline_ = 0;
    void DrainPlayback();
    uint32_t sequence_ = 0;
    size_t max_payload_ = 0;
    bool bootstrap_ = false;
};
