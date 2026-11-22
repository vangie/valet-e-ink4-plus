#include "valet_protocol.h"
#include "valet_wire.h"
#include "valet_crypto/monocypher-ed25519.h"
#include "valet_crypto/passport_ima.h"
#include <fvad.h>
#include "board.h"
#include "display.h"
#include "settings.h"
#include "chat_cloud.h"
#include "application.h"
#include "audio/audio_service.h"
#include "assets/lang_config.h"
#include <esp_random.h>
#include <esp_log.h>
#include "sdkconfig.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <psa/crypto.h>
#include <nvs.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <memory>

namespace {
const char* TAG = "Valet";
uint8_t secret[64]{}, public_key[32]{};
std::string device_id;
std::atomic<bool> live_mode{false}, muted{false};
std::atomic<int> mic_level{0};
std::atomic<int64_t> mic_level_at{0};
std::atomic<bool> mic_voice{false};   // 人声门这会儿开着（真在上传麦克风的声音）
std::atomic<bool> link_reconnecting{false}, call_retrying{false};
#if CONFIG_VALET_DEV_LINK
std::atomic<ValetProtocol*> live_instance{nullptr};
#endif
constexpr size_t kMaxFrame = 65536;
constexpr EventBits_t kHello = BIT0, kFailed = BIT1, kInbox = BIT2, kExit = BIT3, kContext = BIT4, kSubscribed = BIT5, kReconnectExit = BIT6;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using valet::wire::String;
}
// 只在人声门开着时报电平：屋里有杂音时竖条不跟着跳，墨水屏也就不用一直刷。
int ValetProtocol::MicLevel(int64_t* at_us) { if (at_us) *at_us = mic_level_at.load(); return mic_voice.load() ? mic_level.load() : 0; }
bool ValetProtocol::Reconnecting() { return link_reconnecting.load() || call_retrying.load(); }
bool ValetProtocol::InitializeIdentity() {
    nvs_handle_t handle;
    if (nvs_open("valet_identity", NVS_READWRITE, &handle) != ESP_OK) return false;
    uint8_t seed[32]; size_t size = sizeof(seed);
    auto error = nvs_get_blob(handle, "seed", seed, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) { esp_fill_random(seed, sizeof(seed)); error = nvs_set_blob(handle, "seed", seed, sizeof(seed)); if (error == ESP_OK) error = nvs_commit(handle); }
    nvs_close(handle);
    if (error != ESP_OK || size != 32) { crypto_wipe(seed, sizeof(seed)); return false; }
    crypto_ed25519_key_pair(secret, public_key, seed);
    uint8_t hash[32]; size_t hash_size = 0;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_compute(PSA_ALG_SHA_256, public_key, 32, hash, sizeof(hash), &hash_size) != PSA_SUCCESS || hash_size != 32) return false;
    char id[65]{}; for (size_t i = 0; i < 32; ++i) std::snprintf(id + i*2, 3, "%02x", hash[i]);
    device_id = id;
    return true;
}
void ValetProtocol::SetLive(bool enabled) { live_mode.store(enabled); muted.store(false); }
bool ValetProtocol::Live() { return live_mode.load(); }
void ValetProtocol::SetMuted(bool enabled) { muted.store(enabled); }
bool ValetProtocol::Muted() { return muted.load(); }
ValetProtocol::ValetProtocol() : events_(xEventGroupCreate()) { server_sample_rate_ = 24000; server_frame_duration_ = 60; }
ValetProtocol::~ValetProtocol() {
    CloseAudioChannel();
#if CONFIG_VALET_DEV_LINK
    ValetProtocol* self = this; live_instance.compare_exchange_strong(self, nullptr);
#endif
    vEventGroupDelete(events_); if (vad_) fvad_free(vad_);
}
bool ValetProtocol::SendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closing_ || !socket_ || !socket_->IsConnected()) return false;
    // A question may exceed one transport frame; fragment only within this voice task.
    for (size_t offset = 0; offset < text.size(); offset += 16000) {
        if (closing_) return false;
        const auto count = std::min<size_t>(16000, text.size() - offset);
        if (!socket_->Send(text.data()+offset, count, false, offset+count == text.size())) return false;
    }
    return true;
}
bool ValetProtocol::Send(cJSON* root) {
    Json owner(root, cJSON_Delete);
    if (!owner) return false;
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(root), cJSON_free);
    return text && SendText(text.get());
}
void ValetProtocol::Receive(const char* data, size_t length) {
    if (!authenticated_) ESP_LOGI(TAG, "connect: frame %u bytes before login%s", unsigned(length), closing_ ? " (closing, dropped)" : "");
    if (closing_ || !length) return;
    if (length > kMaxFrame) { SetError("网关消息超过设备限制"); xEventGroupSetBits(events_, kFailed); return; }
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    // 助理回话时音频帧来得比播放快，处理线程一时被占着，16 条的上限会把夹在中间的控制回复（比如「已停止输出」）
    // 一起丢掉，通话就再也收不到新回答（2026-11-22 测试里出现）。放宽到 64 条，丢了要留记录。
    if (inbox_.size() >= 64) {
        static int64_t warned = 0;
        if (esp_timer_get_time() - warned > 1000000) { ESP_LOGW(TAG, "inbox full, dropping a frame"); warned = esp_timer_get_time(); }
        xEventGroupSetBits(events_, kFailed); return;
    }
    inbox_.emplace_back(data, length);
    xEventGroupSetBits(events_, kInbox);
}
void ValetProtocol::RunReceiver() {
    while (!closing_) {
        xEventGroupWaitBits(events_, kInbox, pdTRUE, pdFALSE, pdMS_TO_TICKS(20));
        if (!socket_ || !socket_->IsConnected()) { vTaskDelay(1); continue; }
        std::string data;
        for (;;) {
            { std::lock_guard<std::mutex> lock(inbox_mutex_); if (inbox_.empty()) break; data = std::move(inbox_.front()); inbox_.pop_front(); }
            Json root(cJSON_ParseWithLength(data.data(), data.size()), cJSON_Delete);
            if (root) Handle(root.get());
            if (closing_) break;
        }
        DrainPlayback();
    }
    xEventGroupSetBits(events_, kExit);
    vTaskDelete(nullptr);
}
void ValetProtocol::StopReceiver() {
    closing_ = true;
    if (receiver_) { xEventGroupSetBits(events_, kInbox); xEventGroupWaitBits(events_, kExit, pdFALSE, pdFALSE, portMAX_DELAY); receiver_ = nullptr; }
    { std::lock_guard<std::mutex> send(send_mutex_); socket_.reset(); } // Receiver has stopped; join network callbacks before freeing this object.
    std::lock_guard<std::mutex> lock(inbox_mutex_); inbox_.clear();
}
bool ValetProtocol::ConnectOnce(bool bootstrap) {
    StopReceiver(); closing_ = false; authenticated_ = false; bootstrap_ = bootstrap; reached_ = false; refusal_.clear();
    xEventGroupClearBits(events_, kHello | kFailed | kExit | kInbox | kContext | kSubscribed);
    { std::lock_guard<std::mutex> send(send_mutex_); socket_ = Board::GetInstance().GetNetwork()->CreateWebSocket(1); }
    if (!socket_) return false;
    socket_->SetConnectTimeout(connect_timeout_ms_);
    // 登录成功的回复带着网关的状态快照，实测 26.8 KB（2026-11-22，OpenClaw 2026.11.4）；
    // 16 KB 的上限会让它被当成超限断开，设备以为被拒绝。大块内存走 PSRAM。
    socket_->SetReceiveBufferSize(kMaxFrame);
    socket_->OnData([this](const char* data, size_t count, bool binary) { if (!binary) Receive(data, count); });
    socket_->OnDisconnected([this] {
        authenticated_ = false; live_ready_ = false; xEventGroupSetBits(events_, kFailed);
        if (closing_ || reconnecting_) return;   // 重连过程中的失败由重连任务自己处理
        if (opened_ && session_live_) { StartReconnect(); return; }
        SetError("助理连接已断开，请重新进入");
    });
    if (xTaskCreate([](void* arg) { static_cast<ValetProtocol*>(arg)->RunReceiver(); }, "valet_receive", 10240, this, 4, &receiver_) != pdPASS) return false;
    const auto endpoint = valet::wire::Endpoint(profile_.connection.url);
    const int64_t started = esp_timer_get_time();
    if (endpoint.empty() || !socket_->Connect(endpoint.c_str())) {
        ESP_LOGW(TAG, "connect: link not established after %d ms", static_cast<int>((esp_timer_get_time() - started) / 1000));
        return false;
    }
    reached_ = true;
    const int link_ms = static_cast<int>((esp_timer_get_time() - started) / 1000);
    // 等登录结果；重连时按 0.2 秒一段等，挂断了就不再等。
    EventBits_t bits = 0;
    // 登录回复（约 27 KB）正常 0.5 秒内到；连上了却 10 秒没回音，多半是入口卡住。
    for (int waited = 0; waited < 10000 && !(reconnecting_ && stop_reconnect_); waited += 200) {
        bits = xEventGroupWaitBits(events_, kHello | kFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(200));
        if (bits & (kHello | kFailed)) break;
    }
    const bool ok = (bits & kHello) && !(bits & kFailed) && authenticated_;
    ESP_LOGI(TAG, "connect: link %d ms, %s (%s)", link_ms, ok ? "authenticated" : "not authenticated", bootstrap ? "pairing" : "device");
    // 连上了却没等到网关的任何回答（既没放行也没拒绝）：多半是网络或入口卡住，按「连不上」处理并允许重试，
    // 不能当成被拒绝（2026-11-22：这种情况曾显示「网关签名认证失败」，其实网关根本没收到）。
    if (!ok && refusal_.empty() && !(bits & kHello)) reached_ = false;
    return ok;
}
// 连不上（握手超时、网络刚唤醒）再试一次；连上了但网关不认，重试没有意义。
// 只有很快就失败的（比如网络刚唤醒、连接被立刻拒绝）才再试一次；等满时限才失败的，再试多半也一样，
// 直接告诉用户连不上，最长约 8 秒（以前两次各等近 20 秒，要 40 秒才出提示）。
bool ValetProtocol::Connect(bool bootstrap) {
    const int64_t started = esp_timer_get_time();
    if (ConnectOnce(bootstrap)) return true;
    if (reached_ || closing_) return false;
    if (esp_timer_get_time() - started > 3000000) return false;
    ESP_LOGW(TAG, "connect: failed fast, retrying once");
    vTaskDelay(pdMS_TO_TICKS(500));
    return ConnectOnce(bootstrap);
}
ValetProtocol::Pairing ValetProtocol::Refusal() const {
    if (!reached_) return Pairing::Unreachable;
    if (refusal_.find("PAIRING_REQUIRED") != std::string::npos || refusal_ == "NOT_PAIRED") return Pairing::NeedsApproval;
    if (refusal_.find("BOOTSTRAP") != std::string::npos || refusal_.find("PAIRING_EXPIRED") != std::string::npos) return Pairing::CodeExpired;
    if (refusal_.find("SCOPE") != std::string::npos || refusal_.find("PAIRING_REJECTED") != std::string::npos) return Pairing::ScopeDenied;
    return Pairing::Refused;
}
const char* ValetProtocol::PairingText(Pairing result) {
    switch (result) {
        case Pairing::Paired: return "已连接";
        case Pairing::NeedsApproval: return "请在助理那边批准这台设备，批准后会自动连上";
        case Pairing::CodeExpired: return "配对码已过期，请移除这个助理后重新添加";
        case Pairing::ScopeDenied: return "助理那边没有给这台设备权限，请重新配对";
        case Pairing::Unreachable: return "连不上助理，请检查网络后再试一次";
        case Pairing::SaveFailed: return "设备授权保存失败，请重新配对";
        case Pairing::NoProfile: return "请先添加助理";
        default: return "助理拒绝了这次连接，请检查插件版本后重新配对";
    }
}
// 配对码登录：网关在批准之前回 PAIRING_REQUIRED；批准后同一个码（10 分钟内）再登录一次，
// 就能拿到长期凭据。拿到后立刻保存并清掉配对码，之后只用长期凭据。
ValetProtocol::Pairing ValetProtocol::Pair() {
    token_ = valet::wire::PairToken(profile_.connection.code);
    if (token_.empty()) return Pairing::CodeExpired;
    if (!Connect(true)) { const auto why = Refusal(); token_.clear(); return why; }
    auto updated = valet::LoadAssistantProfiles();
    bool found = false;
    for (auto& item : updated.items) if (item.id == profile_.id && item.connection.url == profile_.connection.url) {
        item.device_token = token_; item.connection.code.clear(); item.ask = true; found = true;
    }
    if (!found || !valet::SaveAssistantProfiles(updated)) return Pairing::SaveFailed;
    return Pairing::Paired;
}
ValetProtocol::Pairing ValetProtocol::PairNow(const std::string& profile_id) {
    const auto profiles = valet::LoadAssistantProfiles();
    const valet::AssistantProfile* found = nullptr;
    for (const auto& item : profiles.items) if (item.id == profile_id) found = &item;
    if (!found || device_id.empty()) return Pairing::NoProfile;
    if (!found->device_token.empty()) return Pairing::Paired;
    ValetProtocol once;
    once.profile_ = *found;
    const auto result = once.Pair();
    once.CloseAudioChannel();
    ESP_LOGI(TAG, "pair now: %s", PairingText(result));
    return result;
}
bool ValetProtocol::OpenAudioChannel() {
    const auto profiles = valet::LoadAssistantProfiles();
    if (!profiles.Active() || device_id.empty()) { SetError("请先添加助理，或重新启动设备身份"); return false; }
    session_live_ = Live();
    profile_ = *profiles.Active(); token_ = profile_.device_token;
    error_occurred_ = false;
    context_ = valet::SelectedReadingContext();
    session_id_ = "agent:passport:passport-" + device_id + (context_.Valid() ? "-book-" + context_.book_id : "");
    if (token_.empty()) {
        const auto paired = Pair();
        if (paired != Pairing::Paired) { SetError(PairingText(paired)); CloseAudioChannel(); return false; }
    }
    if (!Connect(false)) {
        SetError(!reached_ ? PairingText(Pairing::Unreachable)
                 : refusal_.find("TOKEN") != std::string::npos || refusal_.find("PAIR") != std::string::npos
                       ? "助理那边已不认这台设备，请移除后重新添加"
                       : "网关签名认证失败，请检查插件版本与设备授权");
        CloseAudioChannel(); return false;
    }
    if (const char* why = Prepare()) { SetError(why); CloseAudioChannel(); return false; }
    pushed_packets_ = 0; ResyncPlayback(); agc_gain_ = 3.0f; agc_env_ = 0.f; live_errors_ = 0; gate_reset_ = true;
    input_decoder_ = std::make_unique<OpusDecoderWrapper>(16000, 1, 60);
    input_resampler_.Configure(16000, 24000);
    output_resampler_.Configure(24000, 16000);
    muted_history_.clear(); barge_pending_ = false; barge_at_ = 0; submit_pending_ = false;
    if (auto* engine = Engine()) { talk_overs_seen_ = engine->TalkOvers(); engine->DropMic(); }
    opened_ = true;
    StartUplink();
#if CONFIG_VALET_DEV_LINK
    live_instance = this;
#endif
    if (on_audio_channel_opened_) on_audio_channel_opened_();
    if (on_connected_) on_connected_();
    return true;
}
const char* ValetProtocol::Prepare() {
    if (context_.Valid() && session_live_ && !book_live_supported_) {
        return "网关未支持带书通话，请升级共享网关";
    }
    if (context_.Valid() && !context_supported_) {
        return "网关未支持阅读上下文，请升级共享网关插件";
    }
    if (context_supported_) {
        context_selected_ = false;
        auto* request = valet::wire::Request("valet-context", "valet.context.set");
        auto* params = cJSON_GetObjectItem(request, "params");
        cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str());
        cJSON_AddItemToObject(params, "context", valet::ContextJson(context_));
        if (!Send(request) || !(xEventGroupWaitBits(events_, kContext | kFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000)) & kContext) || !context_selected_) {
            return "阅读会话选择失败，问题未发送";
        }
    }
    auto* subscribe = valet::wire::Request("valet-subscribe", "sessions.messages.subscribe");
    cJSON_AddStringToObject(cJSON_GetObjectItem(subscribe, "params"), "key", session_id_.c_str());
    if (!Send(subscribe) || !(xEventGroupWaitBits(events_, kSubscribed | kFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000)) & kSubscribed)) {
        return "会话订阅失败，问题未发送";
    }
    return nullptr;
}
void ValetProtocol::CloseAudioChannel() {
    const bool was_open = opened_.exchange(false);
    StopUplink();
    if (auto* engine = Engine()) engine->ArmTalkOver(false);
    JoinReconnect();
    // Join before touching state owned by the receiver.
    StopReceiver();
    recording_ = false; live_ready_ = false; awaiting_ = false; speech_active_ = false; recording_data_.clear(); input_decoder_.reset(); call_id_.clear(); early_ready_.clear();
    marks_.clear(); playback_sequence_ = 0; output_started_ = false; audio_done_ = false; call_pending_ = false; tool_call_.clear(); tool_queue_.clear(); steering_.clear(); finished_tools_.clear();
    captions_.clear(); quiet_since_ = 0; reply_so_far_.clear();
    call_ready_seen_ = false; call_retries_ = 0; recreate_at_ = 0; call_retrying = false;
    if (was_open && on_audio_channel_closed_) on_audio_channel_closed_();
}
bool ValetProtocol::IsAudioChannelOpened() const { return opened_ && (authenticated_ || reconnecting_) && !error_occurred_; }
void ValetProtocol::StartReconnect() {
    if (reconnecting_.exchange(true)) return;
    stop_reconnect_ = false;
    link_reconnecting = true;
    if (reconnector_) {   // 上一次重连留下的任务：等它真正停住再删
        xEventGroupWaitBits(events_, kReconnectExit, pdFALSE, pdFALSE, portMAX_DELAY);
        while (eTaskGetState(reconnector_) != eSuspended) vTaskDelay(1);
        vTaskDeleteWithCaps(reconnector_); reconnector_ = nullptr;
    }
    xEventGroupClearBits(events_, kReconnectExit);
    // 栈放 PSRAM（TLS 握手要 8 KB 左右，内部内存紧）；带 caps 建的任务不能自己删，做完就停住，由下次重连或挂断时删。
    const auto created = xTaskCreateWithCaps([](void* arg) {
        auto* self = static_cast<ValetProtocol*>(arg);
        self->RunReconnect();
        xEventGroupSetBits(self->events_, kReconnectExit);
        vTaskSuspend(nullptr);
    }, "valet_reconnect", 12288, this, 4, &reconnector_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        reconnector_ = nullptr; reconnecting_ = false; link_reconnecting = false;
        SetError("助理连接已断开，请重新进入");
    }
}
void ValetProtocol::JoinReconnect() {
    stop_reconnect_ = true;
    if (!reconnector_) return;
    xEventGroupWaitBits(events_, kReconnectExit, pdFALSE, pdFALSE, portMAX_DELAY);
    while (eTaskGetState(reconnector_) != eSuspended) vTaskDelay(1);
    vTaskDeleteWithCaps(reconnector_);
    reconnector_ = nullptr;
}
void ValetProtocol::RunReconnect() {
    const int64_t started = esp_timer_get_time();
    ESP_LOGW(TAG, "call: link lost, reconnecting");
    {   // 这一段通话已经没了：已说出的半句留在屏幕上，扬声器里剩下的不再播，工具结果也收不到了
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        KeepInterruptedReply();
        for (const auto& caption : captions_)
            Event(caption.user ? "stt" : "tts", caption.user ? nullptr : "sentence_start", caption.text);
        captions_.clear();
        call_id_.clear(); early_ready_.clear(); call_pending_ = false; live_ready_ = false;
        marks_.clear(); output_started_ = false; audio_done_ = false; interrupted_ = false;
        tool_call_.clear(); tool_queue_.clear(); steering_.clear();
        Application::GetInstance().GetAudioService().ResetDecoder(); ResyncPlayback();
        Event("tts", "stop");
    }
    bool ok = false;
    int attempt = 0;
    while (!stop_reconnect_ && esp_timer_get_time() - started < 90000000) {
        ++attempt;
        if (ConnectOnce(false)) {
            const char* why = Prepare();
            if (!why) { ok = true; break; }
            ESP_LOGW(TAG, "call: reconnected but %s", why);
        } else if (reached_ && !refusal_.empty()) {
            ESP_LOGW(TAG, "call: gateway refused the reconnect (%s)", refusal_.c_str());
            break;   // 网关不认了，再试也没用
        }
        // 每次之间等 2 秒（挂断立即停）
        for (int i = 0; i < 10 && !stop_reconnect_; ++i) vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (stop_reconnect_) { reconnecting_ = false; link_reconnecting = false; return; }
    if (ok) {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        live_errors_ = 0; gate_reset_ = true;
        CreateCall();
        ESP_LOGW(TAG, "call: reconnected after %d s (%d attempts)", int((esp_timer_get_time() - started) / 1000000), attempt);
    } else ESP_LOGW(TAG, "call: could not reconnect (%d attempts)", attempt);
    reconnecting_ = false; link_reconnecting = false;
    if (!ok) SetError("助理连接已断开，请重新进入");
}
#if CONFIG_VALET_DEV_LINK
void ValetProtocol::DropLinkForTest() {
    auto* self = live_instance.load();
    if (!self || !self->opened_) { ESP_LOGW(TAG, "droplink: no open channel"); return; }
    std::lock_guard<std::mutex> send(self->send_mutex_);
    if (self->socket_) { ESP_LOGW(TAG, "droplink: closing the link"); self->socket_->Close(); }
}
#endif
void ValetProtocol::Event(const char* type, const char* state, const std::string& text) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "type", type);
    if (state) cJSON_AddStringToObject(root.get(), "state", state);
    if (!text.empty()) cJSON_AddStringToObject(root.get(), "text", text.c_str());
    if (on_incoming_json_) on_incoming_json_(root.get());
}
void ValetProtocol::SendStartListening(ListeningMode) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (session_live_) { if (call_id_.empty() && !call_pending_) CreateCall(); return; }
    if (awaiting_ || speech_active_) SendAbortSpeaking(kAbortReasonNone);
    awaiting_ = false; reply_completed_ = true; run_id_.clear(); request_id_.clear();
    interrupted_ = false;
    {
        std::lock_guard<std::mutex> record(record_mutex_);
        recording_data_.clear(); recording_data_.reserve(16000 * 30 + 58);
    }
    if (input_decoder_) input_decoder_->ResetState();
    if (auto* engine = Engine()) engine->DropMic();   // 只录按下之后的
    submit_pending_ = false;
    recording_ = true;
    ESP_LOGI(TAG, "ask: recording (engine %d, uplink %d)", Engine() ? 1 : 0, uplink_ ? 1 : 0);
}
bool ValetProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (Engine()) return true;   // 新引擎：麦克风由上行任务直接从引擎取（这里收到的 Opus 包不用）
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (!IsAudioChannelOpened()) return false;
    if (session_live_ && (!live_ready_ || Muted())) return true;
    if (!session_live_ && !recording_) return true;
    std::vector<int16_t> pcm;
    if (!input_decoder_ || !input_decoder_->Decode(std::move(packet->payload), pcm)) return false;
    if (session_live_) {
        // 没有回声消除：助理说话时麦克风会收到扬声器的声音，网关把它当成用户插话，
        // 冒出「山」「喂，您好」之类的假提问并打断真正的回答（2026-11-22 实测）。
        // 播放时和播完后 0.5 秒改发静音，打断靠屏幕上的「打断」按钮。
        const int64_t now = esp_timer_get_time();
        // 停顿也算在内：播放中途卡住、或回话声音还攒在缓冲里时扬声器虽然没响，接着还会响；
        // 再从扬声器最后一次出声算起留 0.8 秒（房间余音 + 麦克风这一路的延迟），
        // 否则卡顿那一下麦克风会把助理自己的声音当人声传上去（2026-11-22 出现过「用户：烧。」）。
        auto& audio = Application::GetInstance().GetAudioService();
        if (!audio.IsPlaybackIdle() || !held_.empty()) mic_muted_until_ = now + 500000;
        int64_t played_at = 0;
        audio.OutputLevel(&played_at);
        if (played_at) mic_muted_until_ = std::max(mic_muted_until_, played_at + 800000);
        {
            double sq = 0;
            for (const auto v : pcm) sq += double(v) * v;
            mic_level.store(pcm.empty() ? 0 : int(std::sqrt(sq / pcm.size())));
            mic_level_at.store(now);
        }
        const bool mic_muted = now < mic_muted_until_;
#if CONFIG_VALET_DEV_LINK
        {   // 仅测试版：每 3 秒记一次麦克风上传情况
            static int64_t since = 0; static unsigned sent = 0, muted = 0; static double sumsq = 0; static uint64_t n = 0;
            if (!since) since = now;
            ++sent; if (mic_muted) ++muted;
            for (const auto v : pcm) sumsq += double(v) * v;
            n += pcm.size();
            if (now - since > 3000000) {
                ESP_LOGI(TAG, "mic upload: %u packets, %u muted, mic rms %.1f dBFS, playback idle %d", sent, muted,
                         20 * std::log10(std::max(1.0, std::sqrt(sumsq / std::max<uint64_t>(1, n))) / 32767.0),
                         Application::GetInstance().GetAudioService().IsPlaybackIdle() ? 1 : 0);
                since = now; sent = muted = 0; sumsq = 0; n = 0;
            }
        }
#endif
        std::vector<std::vector<int16_t>> frames;
        mic_voice.store(GateMic(pcm, now, mic_muted, frames));
        for (const auto& frame : frames) if (!SendLiveFrame(frame, call_id_)) return false;
        return true;
    }
    if (recording_data_.size() + pcm.size() > 16000 * 30) { recording_ = false; SetError("录音最多 30 秒，请松手后重新提问"); return false; }
    for (auto sample : pcm) recording_data_.push_back(valet::wire::MuLaw(sample));
    return true;
}
bool g_valet_mic_gate = true;   // 仅测试版可用 micgate 命令关掉做对比
bool ValetProtocol::GateMic(const std::vector<int16_t>& pcm, int64_t now, bool muted, std::vector<std::vector<int16_t>>& out) {
    constexpr int kChunk = 480;                   // fvad 在 16 kHz 下按 30 ms 一段判断
    constexpr size_t kPreroll = 5;                // 开门时补上前面约 0.3 秒
    constexpr int64_t kHangUs = 700000;           // 最后一次听到人声后再开 0.7 秒
    // 开门要够响：2026-11-22 实测屋里远处的说话声 -31～-44 dBFS 也被判成人声，开了门，
    // 助理就对着杂音接着讲故事；对着设备说话在 -5～-25 dBFS。开门后放宽，句尾轻声不被切掉。
    constexpr float kOpenAboveFloorDb = 12.f, kOpenMinDb = -30.f;
    constexpr float kKeepAboveFloorDb = 6.f, kKeepMinDb = -45.f;
    if (gate_reset_ || !vad_) {
        if (!vad_) vad_ = fvad_new();
        // fvad_reset 会把采样率退回 8 kHz、模式退回 0，所以每次重置后都要重新设置；
        // 否则 480 点一段在 8 kHz 下不合法，fvad_process 永远返回 -1（2026-11-22 踩过）。
        if (vad_) { fvad_reset(vad_); fvad_set_sample_rate(vad_, 16000); fvad_set_mode(vad_, 3); }
        gate_preroll_.clear(); gate_open_ = false; gate_score_ = 0; gate_floor_db_ = -60.f; gate_reset_ = false;
    }
    const auto silent = [&] { out.emplace_back(pcm.size(), 0); };
    if (muted) {   // 扬声器在响：麦克风里是助理自己的声音，不更新底噪，也不留作开门前的补音
        if (gate_open_) ESP_LOGI(TAG, "mic gate: closed by playback");
        gate_preroll_.clear(); gate_open_ = false; gate_score_ = 0;
        muted_history_.push_back(pcm);   // 万一是插话，这几包就是句子开头
        if (muted_history_.size() > 6) muted_history_.pop_front();
        silent(); return false;
    }
    if (barge_pending_) {   // 刚判定插话：补上静音期间最后约 0.36 秒，人声门直接打开
        barge_pending_ = false;
        ESP_LOGI(TAG, "mic gate: open by talk-over, %u packets of lead-in", unsigned(muted_history_.size()));
        for (auto& frame : muted_history_) out.push_back(std::move(frame));
        muted_history_.clear(); gate_preroll_.clear();
        gate_open_ = true; gate_score_ = 3; gate_last_voice_ = now;
        out.push_back(pcm); return true;
    }
    muted_history_.clear();
    if (!vad_ || !g_valet_mic_gate) { out.push_back(pcm); return true; }
    double sq = 0;
    for (const auto v : pcm) sq += double(v) * v;
    const float db = 20.f * std::log10(std::max(1.0, std::sqrt(sq / std::max<size_t>(1, pcm.size()))) / 32767.f);
    int chunks = 0, voiced = 0;
    for (size_t off = 0; off + kChunk <= pcm.size(); off += kChunk, ++chunks)
        if (fvad_process(vad_, pcm.data() + off, kChunk) == 1) ++voiced;
    const bool voice = chunks && voiced * 2 >= chunks;
    const bool speech = voice && (gate_open_ ? db > std::max(gate_floor_db_ + kKeepAboveFloorDb, kKeepMinDb)
                                             : db > std::max(gate_floor_db_ + kOpenAboveFloorDb, kOpenMinDb));
    // 底噪：比现在低就很快跟下去，比现在高只慢慢涨（约 6 秒），有人说话时不涨。
    if (db < gate_floor_db_) gate_floor_db_ += (db - gate_floor_db_) * 0.3f;
    else if (!speech) gate_floor_db_ += (db - gate_floor_db_) * 0.01f;
    gate_floor_db_ = std::max(gate_floor_db_, -75.f);
    gate_score_ = speech ? std::min(gate_score_ + 1, 3) : std::max(gate_score_ - 1, 0);
    if (speech) gate_last_voice_ = now;
    if (!gate_open_ && gate_score_ >= 2) {
        gate_open_ = true;
        ESP_LOGI(TAG, "mic gate: open, %.1f dBFS, floor %.1f, preroll %u", double(db), double(gate_floor_db_), unsigned(gate_preroll_.size()));
        for (auto& frame : gate_preroll_) out.push_back(std::move(frame));
        gate_preroll_.clear();
    } else if (gate_open_ && now - gate_last_voice_ > kHangUs) {
        gate_open_ = false;
        ESP_LOGI(TAG, "mic gate: closed, floor %.1f dBFS", double(gate_floor_db_));
    }
    if (gate_open_) { out.push_back(pcm); return true; }
    gate_preroll_.push_back(pcm);
    if (gate_preroll_.size() > kPreroll) gate_preroll_.pop_front();
    silent(); return false;
}
bool ValetProtocol::SendLiveFrame(const std::vector<int16_t>& pcm, const std::string& call_id) {
    {
        std::vector<int16_t> resampled(input_resampler_.GetOutputSamples(pcm.size()));
        input_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
        auto* root = valet::wire::Request(("valet-a-" + std::to_string(++sequence_)).c_str(), "talk.session.appendAudio");
        auto* params = cJSON_GetObjectItem(root, "params"); cJSON_AddStringToObject(params, "sessionId", call_id.c_str());
        std::vector<uint8_t> compressed;
        int index = 0;
        for (size_t offset = 0; offset < resampled.size(); offset += PASSPORT_IMA_SAMPLES) {
            uint8_t block[PASSPORT_IMA_BYTES];
            const auto count = std::min<size_t>(PASSPORT_IMA_SAMPLES, resampled.size() - offset);
            const auto bytes = passport_ima_encode(resampled.data() + offset, count, &index, block, sizeof(block));
            if (!bytes) { cJSON_Delete(root); return false; }
            compressed.insert(compressed.end(), block, block + bytes);
        }
        const auto audio = "passport-ima:" + valet::wire::Base64(compressed.data(), compressed.size());
        cJSON_AddStringToObject(params, "audioBase64", audio.c_str());
        return Send(root);
    }
}
VoiceEngine* ValetProtocol::Engine() {
    auto& audio = Application::GetInstance().GetAudioService();
    return audio.EngineMode() && audio.Engine().Running() ? &audio.Engine() : nullptr;
}
void ValetProtocol::StartUplink() {
    if (uplink_ || !Engine()) return;
    if (!uplink_done_) uplink_done_ = xSemaphoreCreateBinary();
    xSemaphoreTake(uplink_done_, 0);
    uplink_stop_ = false;
    // 要发 TLS、拼 JSON：栈 8 KB 放 PSRAM（内部内存紧），带 caps 建的任务做完就停住，由 StopUplink 删。
    if (xTaskCreateWithCaps([](void* arg) {
            auto* self = static_cast<ValetProtocol*>(arg);
            self->RunUplink();
            xSemaphoreGive(self->uplink_done_);
            vTaskSuspend(nullptr);
        }, "valet_uplink", 8192, this, 5, &uplink_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        uplink_ = nullptr;
        ESP_LOGE(TAG, "uplink task create failed");
    }
}
void ValetProtocol::StopUplink() {
    if (!uplink_) return;
    uplink_stop_ = true;
    if (xSemaphoreTake(uplink_done_, pdMS_TO_TICKS(3000)) != pdTRUE) ESP_LOGW(TAG, "uplink did not stop in time");
    while (eTaskGetState(uplink_) != eSuspended) vTaskDelay(1);
    vTaskDeleteWithCaps(uplink_);
    uplink_ = nullptr;
}
void ValetProtocol::RunUplink() {
    std::vector<int16_t> pcm;
    pcm.reserve(960);
    int16_t frame[VoiceEngine::kFrame];
    const auto submit = [this] {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        if (submit_pending_.exchange(false)) {
            ESP_LOGI(TAG, "ask: uploading %u samples", unsigned(recording_data_.size()));
            Submit();
        }
    };
    while (!uplink_stop_) {
        VoiceEngine* engine = Engine();
        if (!engine) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        int64_t at = 0;
        if (!engine->ReadMic(frame, 60, &at)) {
            if (submit_pending_) submit();
            continue;
        }
        if (!session_live_) {   // 提问：按住期间录下来（μ-law），松开后把松开前录到的取完再上传
            if (submit_pending_ && at > submit_at_us_) { submit(); continue; }
            if (!recording_ && !submit_pending_) continue;
            std::lock_guard<std::mutex> record(record_mutex_);
            if (recording_data_.size() + VoiceEngine::kFrame > 16000 * 30) {
                if (recording_.exchange(false)) SetError("录音最多 30 秒，请松手后重新提问");
                continue;
            }
            for (int i = 0; i < VoiceEngine::kFrame; ++i) recording_data_.push_back(valet::wire::MuLaw(frame[i]));
            continue;
        }
        pcm.insert(pcm.end(), frame, frame + VoiceEngine::kFrame);
        if (pcm.size() < 960) continue;   // 凑 60 ms 一包：人声门按 30 ms 判断，网关按包收
        UplinkLive(pcm, at, *engine);
        pcm.clear();
    }
}
void ValetProtocol::UplinkLive(const std::vector<int16_t>& pcm, int64_t now, VoiceEngine& engine) {
    bool ready = false, output_started = false;
    std::string call;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        if (!IsAudioChannelOpened()) { engine.ArmTalkOver(false); return; }
        ready = live_ready_ && !reconnecting_; output_started = output_started_; call = call_id_;
    }
    const bool muted_by_user = Muted();
    engine.ArmTalkOver(ready && !muted_by_user);
    {
        double sq = 0;
        for (const auto v : pcm) sq += double(v) * v;
        mic_level.store(int(std::sqrt(sq / pcm.size())));
        mic_level_at.store(now);
    }
    if (!ready || muted_by_user || call.empty()) return;
    // 扬声器在响：播放环里还有声音，或最后一次出声之后还没过「路径延迟 + 300 ms 余音」
    const bool speaker = !engine.PlaybackIdle() ||
                         now - engine.LastSoundUs() < int64_t(VoiceEngine::kPathDelayMs + 300) * 1000;
    const uint32_t overs = engine.TalkOvers();
    if (overs != talk_overs_seen_) {
        talk_overs_seen_ = overs;
        if (speaker) {
            ESP_LOGI(TAG, "call: user spoke over the reply, interrupting");
            Application::GetInstance().AbortSpeaking(kAbortReasonNone);   // 清空播放环 + cancelOutput
            barge_at_ = now; barge_pending_ = true;
        }
    }
    // 插话之后，只要用户还在说（人声门开着）就一直不静音，新回答开始了也不静音，直到这句话说完。
    // 2026-11-23：原来只放开 1.5 秒，网关在用户句中的停顿处就开始回答，麦克风随即又被静音，
    // 「等一下，我想 / 问另外一个问题」被切成两段，后半句丢了。
    if (barge_at_ && (now - barge_at_ > 15000000 || (now - barge_at_ > 1500000 && !gate_open_))) barge_at_ = 0;
    const bool after_barge = barge_at_ != 0;
    (void)output_started;
    std::vector<std::vector<int16_t>> frames;
    mic_voice.store(GateMic(pcm, now, speaker && !after_barge, frames));
    for (const auto& f : frames) if (!SendLiveFrame(f, call)) break;
#if CONFIG_VALET_DEV_LINK
    {   // 仅测试版：每 3 秒记一次上行情况
        static int64_t since = 0; static unsigned sent = 0, muted = 0;
        if (!since) since = now;
        ++sent; if (speaker && !after_barge) ++muted;
        if (now - since > 3000000) {
            const auto s = engine.TakeStats();
            ESP_LOGI(TAG, "uplink: %u packets, %u muted, mic rms %d; engine: %u frames, ran dry %u, mic dropped %u, worst loop %d ms",
                     sent, muted, mic_level.load(), unsigned(s.frames), unsigned(s.underruns), unsigned(s.mic_dropped), s.max_loop_ms);
            since = now; sent = muted = 0;
        }
    }
#endif
}
bool ValetProtocol::Submit() {
    std::unique_lock<std::mutex> record(record_mutex_);
    if (recording_data_.empty()) { SetError("没有收到录音，请重试"); return false; }
    // Move the recording into its WAV envelope, then free it before serializing JSON.
    const auto audio = [this] {
        const auto wav = valet::wire::Wav(std::move(recording_data_));
        recording_data_.clear();
        return valet::wire::Base64(wav.data(), wav.size());
    }();
    record.unlock();
    if (!max_payload_ || audio.size() + 1024 > max_payload_) { SetError("录音超过网关限制，请缩短后重试"); return false; }
    run_id_.clear(); reply_.clear(); reply_completed_ = false; interrupted_ = false;
    awaiting_ = true; ask_deadline_ = esp_timer_get_time() + 180000000; last_poll_ = 0;
    request_id_ = "valet-" + std::to_string(esp_random()) + "-" + std::to_string(++sequence_);
    auto* root = valet::wire::Request(request_id_.c_str(), "chat.send"); auto* params = cJSON_GetObjectItem(root, "params");
    cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str()); cJSON_AddStringToObject(params, "message", "");
    cJSON_AddStringToObject(params, "thinking", "off"); cJSON_AddBoolToObject(params, "fastMode", true);
    cJSON_AddStringToObject(params, "idempotencyKey", request_id_.c_str());
    auto* attachments = cJSON_AddArrayToObject(params, "attachments"); auto* attachment = cJSON_CreateObject(); cJSON_AddItemToArray(attachments, attachment);
    cJSON_AddStringToObject(attachment, "type", "audio"); cJSON_AddStringToObject(attachment, "mimeType", "audio/wav");
    cJSON_AddStringToObject(attachment, "fileName", "valet.wav"); // The reference stays valid through synchronous Send; avoid another large copy.
    cJSON_AddItemToObject(attachment, "content", cJSON_CreateStringReference(audio.c_str()));
    if (Send(root)) return true;
    awaiting_ = false; SetError("提问发送失败，请重新连接助理"); return false;
}
void ValetProtocol::SendStopListening() {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (!recording_.exchange(false)) { ESP_LOGW(TAG, "ask: released but was not recording"); return; }
    ESP_LOGI(TAG, "ask: released, %u samples so far", unsigned(recording_data_.size()));
    // 新引擎：上行任务把松开之前录到的帧取完，再上传
    if (Engine()) { submit_at_us_ = esp_timer_get_time(); submit_pending_ = true; return; }
    Submit();
    std::lock_guard<std::mutex> record(record_mutex_);
    recording_data_.clear();
}
void ValetProtocol::CreateCall() {
    call_pending_ = true; call_ready_seen_ = false; recreate_at_ = 0;
    playback_sequence_ = 0;   // 每个新通话的音频序号都从头算；留着旧通话的序号会把新回答的声音全当成过期包丢掉
    auto* root = valet::wire::Request("valet-call", "talk.session.create"); auto* params = cJSON_GetObjectItem(root, "params");
    cJSON_AddStringToObject(params, "mode", "realtime"); cJSON_AddStringToObject(params, "transport", "gateway-relay");
    cJSON_AddStringToObject(params, "brain", "agent-consult"); cJSON_AddStringToObject(params, "language", std::strcmp(Lang::Current(), "zh-CN") == 0 ? "zh" : "en");
    cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str());
    Send(root);
}
void ValetProtocol::SendAbortSpeaking(AbortReason) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    marks_.clear(); audio_done_ = false; output_started_ = false; tool_call_.clear(); tool_queue_.clear();
    interrupted_ = true; awaiting_ = false; interrupt_at_ = esp_timer_get_time();
    KeepInterruptedReply();
    Application::GetInstance().GetAudioService().ResetDecoder(); ResyncPlayback();
    if (speech_active_) { Send(valet::wire::Request("valet-speech-cancel", "valet.speech.cancel")); speech_active_ = false; }
    auto* root = valet::wire::Request("valet-interrupt", session_live_ ? "talk.session.cancelOutput" : "chat.abort");
    auto* params = cJSON_GetObjectItem(root, "params");
    if (session_live_) cJSON_AddStringToObject(params, "sessionId", call_id_.c_str());
    else { cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str()); cJSON_AddStringToObject(params, "runId", run_id_.c_str()); }
    Send(root);
}
void ValetProtocol::Handle(cJSON* root) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    const auto* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const std::string event = String(root, "event"), id = String(root, "id");
    if (event == "connect.challenge") {
        const auto* timestamp = cJSON_GetObjectItemCaseSensitive(payload, "ts");
        const bool sent = cJSON_IsNumber(timestamp) && timestamp->valuedouble >= 0 &&
            Send(valet::wire::Connect(secret, public_key, device_id, token_, bootstrap_, String(payload, "nonce"), timestamp->valuedouble));
        ESP_LOGI(TAG, "connect: challenge received, login %s", sent ? "sent" : "NOT sent");
        if (!sent) xEventGroupSetBits(events_, kFailed);
        return;
    }
    if (id == "valet-connect") {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
            const auto* error = cJSON_GetObjectItemCaseSensitive(root, "error");
            refusal_ = String(cJSON_GetObjectItemCaseSensitive(error, "details"), "code");
            if (refusal_.empty()) refusal_ = String(error, "code");
            if (refusal_.empty()) refusal_ = "REFUSED";
            ESP_LOGW(TAG, "connect refused: %s", refusal_.c_str());
            xEventGroupSetBits(events_, kFailed); return;
        }
        const auto* version = cJSON_GetObjectItemCaseSensitive(payload, "passportProtocol");
        if (!cJSON_IsNumber(version) || version->valueint != 1 || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "passportAsk"))) { xEventGroupSetBits(events_, kFailed); return; }
        const auto* policy = cJSON_GetObjectItemCaseSensitive(payload, "policy");
        const auto* max = cJSON_GetObjectItemCaseSensitive(policy, "maxPayload"); max_payload_ = cJSON_IsNumber(max) ? max->valueint : 0;
        const auto* auth = cJSON_GetObjectItemCaseSensitive(payload, "auth");
        std::string token = String(auth, "deviceToken");
        const cJSON* item; cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(auth, "deviceTokens")) if (!std::strcmp(String(item, "role"), "operator")) token = String(item, "deviceToken");
        if (bootstrap_ && (token.empty() || token.size() > 127)) { xEventGroupSetBits(events_, kFailed); return; }
        if (!token.empty()) token_ = token;
        authenticated_ = true;
        const auto* feature = cJSON_GetObjectItemCaseSensitive(payload, "valetBookContext");
        context_supported_ = cJSON_IsNumber(feature) && feature->valueint == 1;
        book_live_supported_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "valetBookLive"));
        const auto* speech = cJSON_GetObjectItemCaseSensitive(payload, "valetAskSpeech");
        speech_supported_ = cJSON_IsNumber(speech) && speech->valueint == 1;
        xEventGroupSetBits(events_, kHello); return;
    }
    if (!authenticated_) return;
    if (!request_id_.empty() && id == "valet-speech-" + request_id_) {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) { speech_active_ = false; Event("tts", "stop"); }
        return;
    }
    if (id == "valet-subscribe") { xEventGroupSetBits(events_, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")) ? kSubscribed : kFailed); return; }
    if (id == "valet-context") { context_selected_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")) && String(payload, "sessionKey") == session_id_; xEventGroupSetBits(events_, context_selected_ ? kContext : kFailed); return; }
    if (id.rfind("valet-a-", 0) == 0 && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
        live_ready_ = false; SetError("网关拒绝实时音频，请检查插件格式"); return;
    }
    if (id == "valet-call") {
        call_pending_ = false;
        const auto* audio = cJSON_GetObjectItemCaseSensitive(payload, "audio");
        const auto* rate = cJSON_GetObjectItemCaseSensitive(audio, "outputSampleRateHz");
        const auto* input_rate = cJSON_GetObjectItemCaseSensitive(audio, "inputSampleRateHz");
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")) || std::strcmp(String(audio, "outputEncoding"), "pcm16") || !cJSON_IsNumber(rate) || rate->valueint != 24000 || std::strcmp(String(audio, "inputEncoding"), "pcm16") || !cJSON_IsNumber(input_rate) || input_rate->valueint != 24000) { SetError("实时语音未配置或格式不兼容"); return; }
        call_id_ = String(payload, "sessionId"); live_ready_ = !call_id_.empty() && early_ready_ == call_id_; return;
    }
    if (id == "valet-interrupt") { if (session_live_) interrupted_ = false; interrupt_at_ = 0; Event("tts", "stop"); return; }
    if (id == "valet-transcript" && awaiting_ && String(payload, "runId") == run_id_) {
        if (*String(payload, "text") && reply_.empty()) Event("stt", nullptr, String(payload, "text"));
        reply_ = String(payload, "text"); return;
    }
    if (id == "valet-history" && awaiting_ && !interrupted_) {
        const cJSON* message;
        cJSON_ArrayForEach(message, cJSON_GetObjectItemCaseSensitive(payload, "messages")) {
            const auto* meta = cJSON_GetObjectItemCaseSensitive(message, "__openclaw");
            if (String(meta, "runId") != run_id_ || std::strcmp(String(message, "role"), "assistant")) continue;
            const std::string stop = String(message, "stopReason");
            if (stop == "error" || stop == "aborted") { awaiting_ = false; SetError("助理处理失败，请重试"); return; }
            if (stop == "stop" || stop == "end_turn") {
                awaiting_ = false;
                const auto text = valet::wire::MessageText(message);
                CompleteReply(text); return;
            }
        }
        return;
    }
    for (auto it = steering_.begin(); it != steering_.end(); ++it) if (it->first == id) {
        const std::string call = it->second;
        steering_.erase(it);
        const bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
        std::string text = String(payload, "message");
        if (text.empty()) text = ok ? "已转告正在处理的任务" : "没能转告正在处理的任务";
        ESP_LOGI(TAG, "tool control done: %s", ok ? "ok" : "failed");
        auto* result = valet::wire::Request("valet-tool-result", "talk.session.submitToolResult");
        auto* params = cJSON_GetObjectItem(result, "params");
        cJSON_AddStringToObject(params, "sessionId", call_id_.c_str()); cJSON_AddStringToObject(params, "callId", call.c_str());
        cJSON_AddStringToObject(cJSON_AddObjectToObject(params, "result"), ok ? "result" : "error", text.c_str());
        Send(result);
        return;
    }
    if (id == "valet-tool") {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) { FinishTool("error", "工具调用没有被网关接受"); return; }
        tool_run_ = String(payload, "runId"); tool_key_ = String(payload, "agentSessionKey"); return;
    }
    if (!tool_call_.empty() && event == "chat" && String(payload, "sessionKey") == tool_key_ && String(payload, "runId") == tool_run_) {
        const std::string status = String(payload, "state");
        if (status == "final" || status == "error" || status == "aborted") {
            FinishTool(status == "final" ? "result" : "error", valet::wire::MessageText(cJSON_GetObjectItemCaseSensitive(payload, "message")));
        }
        return;
    }
    if (!request_id_.empty() && id == request_id_) {
        if (!awaiting_ || interrupted_) return;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) { awaiting_ = false; SetError("助理拒绝了提问，请检查插件"); }
        else run_id_ = String(payload, "runId");
        return;
    }
    if (event == "chat" && awaiting_ && !session_live_ && !interrupted_ && String(payload, "sessionKey") == session_id_ &&
        (!run_id_.empty() && String(payload, "runId") == run_id_)) {
        const std::string status = String(payload, "state");
        if (status == "final") { awaiting_ = false; auto text = valet::wire::MessageText(cJSON_GetObjectItemCaseSensitive(payload, "message")); CompleteReply(text); }
        else if (status == "error" || status == "aborted") { awaiting_ = false; SetError("助理处理失败，请重新提问"); }
        return;
    }
    const bool speech_event = event == "valet.speech";
    if (speech_event && (!speech_active_ || interrupted_ || String(payload, "sessionKey") != session_id_ || String(payload, "runId") != run_id_)) return;
    if (speech_event) {
        const std::string type = String(payload, "type");
        if (type == "error") { speech_active_ = false; Event("tts", "stop");
            Application::GetInstance().Schedule([] { auto* display = Board::GetInstance().GetDisplay(); display->ShowNotification("语音播报不可用，文字答案已保留"); });
            return; }
        if (type == "start") { Event("tts", "start"); return; }
        if (type == "done") { audio_done_ = true; if (!output_started_) { speech_active_ = false; Event("tts", "stop"); } return; }
        const auto* sequence = cJSON_GetObjectItemCaseSensitive(payload, "sequence");
        if (type != "audio" || !cJSON_IsNumber(sequence) || sequence->valuedouble <= speech_sequence_) return;
        speech_sequence_ = sequence->valuedouble;
    }
    if (event != "talk.event" && !speech_event) return;
    const std::string type = String(payload, "type"), owner = String(payload, "relaySessionId");
    if (type == "ready") { early_ready_ = owner; if (owner == call_id_) live_ready_ = true; }
    if (!speech_event && (call_id_.empty() || owner != call_id_)) return;
    if (type == "close") {
        live_ready_ = false;
        if (!call_ready_seen_ && call_retries_ < 3 && opened_) {
            ++call_retries_;
            ESP_LOGW(TAG, "call: closed before it was ready, creating it again (%d/3)", call_retries_);
            call_id_.clear(); early_ready_.clear();
            recreate_at_ = esp_timer_get_time() + 2000000; call_retrying = true;
            return;
        }
        call_retrying = false;
        SetError("通话已结束"); return;
    }
    if (type == "error") {
        // 和徽章一样：只有回答阶段的错误、或者错误一再出现，才结束通话。其余（比如 OpenAI 的
        // active-response：上一个回答还没完，新的回答请求被忽略）不影响通话，记下来继续
        // （2026-11-22：以前一律结束通话，助理说完第一句后第二句就没人听了）。
        const std::string phase = String(payload, "phase");
        ESP_LOGW(TAG, "live error (%d): phase=%s code=%s %.80s", live_errors_ + 1, phase.c_str(),
                 String(payload, "code"), String(payload, "message"));
        if (phase == "response" || ++live_errors_ > 5) { live_ready_ = false; SetError("实时服务不可用，请检查助理配置"); }
        return;
    }
    // 打断后不再播旧回答的声音，但它的文字照样记下：屏幕上要留着助理刚才说到哪儿。
    if (interrupted_ && type != "transcript" && type != "partial") return;
    const auto* playback = cJSON_GetObjectItemCaseSensitive(payload, "playbackSequence");
    if (cJSON_IsNumber(playback)) {
        if (playback->valuedouble <= playback_sequence_) return;
        playback_sequence_ = playback->valuedouble;
    }
    if (type == "mark") {
        const std::string name = String(payload, "markName");
        if (name.empty() || name.size() > 128) return;
        // 网关的确认点按顺序编号，确认后一个就隐含确认了前一个；攒满时用新的替换最后一个，
        // 而不是报错（2026-11-22：助理连说两段时扬声器一直不空，旧代码攒满 16 个就把通话打断）。
        // 新引擎按样本：确认点 = 它到达时已经放进播放环的样本数，播到这里才回
        const uint64_t target = Engine() ? Engine()->Queued() : pushed_packets_ + held_.size();
        if (marks_.size() >= 16) marks_.back() = {name, target};
        else marks_.push_back({name, target});
        return;
    }
    if (type == "toolCall") {
        const std::string name = String(payload, "name"), call_id = String(payload, "callId");
        if (call_id.empty() || call_id == tool_call_) return;
        for (const auto& done : finished_tools_) if (done == call_id) return;
        for (const auto& waiting : tool_queue_) if (waiting.call_id == call_id) return;
        const auto* raw = cJSON_GetObjectItemCaseSensitive(payload, "args");
        Json parsed(cJSON_IsString(raw) ? cJSON_Parse(raw->valuestring) : nullptr, cJSON_Delete);   // 有的版本把参数发成字符串
        const cJSON* args = parsed ? parsed.get() : raw;
        const auto reject = [&](const char* why) {   // 不认识的工具：告诉助理，不挂断
            ESP_LOGW(TAG, "tool call %s rejected: %s", name.c_str(), why);
            auto* result = valet::wire::Request("valet-tool-result", "talk.session.submitToolResult");
            auto* params = cJSON_GetObjectItem(result, "params");
            cJSON_AddStringToObject(params, "sessionId", call_id_.c_str()); cJSON_AddStringToObject(params, "callId", call_id.c_str());
            cJSON_AddStringToObject(cJSON_AddObjectToObject(params, "result"), "error", why);
            Send(result);
        };
        if (!cJSON_IsObject(args)) { reject("Invalid tool arguments"); return; }
        if (name == "openclaw_agent_control") {
            const char* text = String(args, "text");
            if (!*text) { reject("Missing text"); return; }
            const std::string request_id = "valet-steer-" + std::to_string(++sequence_);
            auto* request = valet::wire::Request(request_id.c_str(), "talk.session.steer");
            auto* params = cJSON_GetObjectItem(request, "params");
            cJSON_AddStringToObject(params, "sessionId", call_id_.c_str()); cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str());
            cJSON_AddStringToObject(params, "text", text);
            if (*String(args, "mode")) cJSON_AddStringToObject(params, "mode", String(args, "mode"));
            ESP_LOGI(TAG, "tool control (%s)", String(args, "mode"));
            if (steering_.size() >= 4) steering_.pop_front();
            steering_.push_back({request_id, call_id});
            Send(request);
            return;
        }
        if (name != "openclaw_agent_consult") { reject("Unsupported voice tool"); return; }
        char* text = cJSON_PrintUnformatted(args);
        const std::string args_json = text ? text : "{}";
        cJSON_free(text);
        ESP_LOGI(TAG, "tool call %s (%u queued before it)", name.c_str(), unsigned(tool_queue_.size() + !tool_call_.empty()));
        if (tool_call_.empty()) StartTool(call_id, name, args_json);
        else if (tool_queue_.size() < 4) tool_queue_.push_back({call_id, name, args_json});
        else {   // 排太多了：直接告诉助理，让它自己说
            auto* result = valet::wire::Request("valet-tool-result", "talk.session.submitToolResult");
            auto* params = cJSON_GetObjectItem(result, "params");
            cJSON_AddStringToObject(params, "sessionId", call_id_.c_str()); cJSON_AddStringToObject(params, "callId", call_id.c_str());
            cJSON_AddStringToObject(cJSON_AddObjectToObject(params, "result"), "error", "前面的请求还在处理，请稍后再试");
            Send(result);
        }
        return;
    }
    if (type == "transcript" || type == "partial") {
        const bool final_text = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "final"));
        if (session_live_ && !std::strcmp(String(payload, "role"), "assistant")) {
            if (final_text) reply_so_far_.clear();
            else if (!interrupted_ && reply_so_far_.size() < 4096) reply_so_far_ += String(payload, "text");
        }
        if (final_text) {
            const bool user = !std::strcmp(String(payload, "role"), "user");
            // 通话里字幕先攒着，等扬声器停下来再一起上屏：墨水屏刷一次要占满两个核约 0.4 秒，
            // 助理说话时刷字幕，播放抢不到 CPU 就断音（2026-11-22 实测）。
            if (session_live_) captions_.push_back({user, String(payload, "text")});
            else Event(user ? "stt" : "tts", user ? nullptr : "sentence_start", String(payload, "text"));
        }
    } else if (type == "audio") {
        std::string encoded = String(payload, "audioBase64"); const bool ima = encoded.rfind("passport-ima:", 0) == 0;
        auto bytes = valet::wire::Unbase64(ima ? encoded.substr(13) : encoded);
        std::vector<int16_t> pcm;
        if (ima) {
            for (size_t offset = 0; offset < bytes.size();) {
                if (bytes.size()-offset < 6) { SetError("实时音频损坏"); return; }
                const size_t count = bytes[offset+4] | (size_t(bytes[offset+5]) << 8), length = 6 + count/2;
                if (length > bytes.size()-offset || count > 480) { SetError("实时音频损坏"); return; }
                int16_t block[480]; const auto n = passport_ima_decode(bytes.data()+offset, length, block, 480);
                if (!n) { SetError("实时音频损坏"); return; }
                pcm.insert(pcm.end(), block, block+n); offset += length;
            }
        } else { if (bytes.size()%2) return; pcm.resize(bytes.size()/2); std::memcpy(pcm.data(), bytes.data(), bytes.size()); }
        for (const auto v : pcm) { const int a = v < 0 ? -int(v) : int(v); if (a > level_peak_) level_peak_ = a; level_sumsq_ += uint64_t(int64_t(v) * v); }
        ApplyReplyGain(pcm);
        level_samples_ += pcm.size();
        if (!output_started_) { output_started_ = true; Event("tts", "start"); }
        audio_done_ = false;
        if (auto* engine = Engine()) {
            // 直接放进引擎的播放环（不阻塞收消息的线程）；提问模式按设置决定播不播回答的语音
            if (session_live_ || chat_cloud::SpeechPlaybackEnabled()) {
                std::vector<int16_t> out(output_resampler_.GetOutputSamples(pcm.size()));
                output_resampler_.Process(pcm.data(), pcm.size(), out.data());
                engine->Play(out.data(), out.size());
            }
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>(); packet->sample_rate = 24000; packet->pcm = true;
        packet->payload.resize(pcm.size()*2); std::memcpy(packet->payload.data(), pcm.data(), packet->payload.size());
        PushReplyAudio(std::move(packet));
    } else if (type == "audioDone") { audio_done_ = true; ReleaseHeldAudio(); }
    else if (type == "clear") {
        KeepInterruptedReply();   // 网关因为用户插话清掉了这段回答
        marks_.clear(); output_started_ = false; audio_done_ = false;
        Application::GetInstance().GetAudioService().ResetDecoder(); ResyncPlayback();
        Event("tts", "stop");
    }
}

void ValetProtocol::ApplyReplyGain(std::vector<int16_t>& pcm) {
    // 第二版（2026-11-22）：第一版按每包峰值把倍数「一步压到安全值」，倍数在包与包之间跳变，
    // 再加上倍数跟着音节轻重变化，听起来像声音在颤。现在：
    // - 倍数只跟着约 2 秒的平均响度慢慢走（0.5～5 倍），每包内线性过渡，绝不跳变；
    // - 防破音交给无状态的软限幅：超过 kKnee 的部分用 tanh 渐近满幅，永远不会削波，
    //   也不会因为一个响的音节而改变后面的音量。
    constexpr float kTarget = 6300.f, kKnee = 22000.f, kFull = 32767.f;
    constexpr float kMin = 0.5f, kMax = 5.0f, kQuiet = 150.f, kWindow = 2.0f, kTau = 2.0f;
    if (pcm.empty()) return;
    double sumsq = 0;
    for (const auto v : pcm) sumsq += double(v) * v;
    const float ms = float(sumsq / pcm.size());
    const float dt = float(pcm.size()) / 24000.f;
    float next = agc_gain_;
    if (std::sqrt(ms) > kQuiet) {
        agc_env_ = agc_env_ > 0.f ? agc_env_ + (ms - agc_env_) * (1.f - std::exp(-dt / kWindow)) : ms;
        const float want = std::clamp(kTarget / std::max(1.f, std::sqrt(agc_env_)), kMin, kMax);
        next = agc_gain_ + (want - agc_gain_) * (1.f - std::exp(-dt / kTau));
    }
    const float start = agc_gain_;
    const size_t n = pcm.size();
    for (size_t i = 0; i < n; ++i) {
        const float g = start + (next - start) * float(i + 1) / float(n);
        float y = pcm[i] * g;
        const float a = std::fabs(y);
        if (a > kKnee) y = std::copysign(kKnee + (kFull - kKnee) * std::tanh((a - kKnee) / (kFull - kKnee)), y);
        pcm[i] = static_cast<int16_t>(std::clamp(y, -kFull, kFull));
    }
    agc_gain_ = next;
    agc_gain_min_ = std::min(agc_gain_min_, next); agc_gain_max_ = std::max(agc_gain_max_, next);
}
void ValetProtocol::StartTool(const std::string& call_id, const std::string& name, const std::string& args) {
    tool_call_ = call_id; tool_run_.clear(); tool_key_.clear(); tool_deadline_ = esp_timer_get_time() + 120000000;
    auto* request = valet::wire::Request("valet-tool", "talk.client.toolCall");
    auto* params = cJSON_GetObjectItem(request, "params");
    cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str()); cJSON_AddStringToObject(params, "relaySessionId", call_id_.c_str());
    cJSON_AddStringToObject(params, "callId", call_id.c_str()); cJSON_AddStringToObject(params, "name", name.c_str());
    auto* parsed = cJSON_Parse(args.c_str());
    cJSON_AddItemToObject(params, "args", parsed ? parsed : cJSON_CreateObject());
    Send(request);
}
void ValetProtocol::FinishTool(const char* key, const std::string& text) {
    if (tool_call_.empty()) return;
    ESP_LOGI(TAG, "tool call done: %s, %u waiting", key, unsigned(tool_queue_.size()));
    auto* result = valet::wire::Request("valet-tool-result", "talk.session.submitToolResult");
    auto* params = cJSON_GetObjectItem(result, "params");
    cJSON_AddStringToObject(params, "sessionId", call_id_.c_str()); cJSON_AddStringToObject(params, "callId", tool_call_.c_str());
    cJSON_AddStringToObject(cJSON_AddObjectToObject(params, "result"), key, text.c_str());
    Send(result);
    finished_tools_.push_back(tool_call_);
    if (finished_tools_.size() > 4) finished_tools_.pop_front();
    tool_call_.clear(); tool_run_.clear(); tool_key_.clear();
    if (!tool_queue_.empty()) {
        auto next = std::move(tool_queue_.front());
        tool_queue_.pop_front();
        StartTool(next.call_id, next.name, next.args);
    }
}
void ValetProtocol::KeepInterruptedReply() {
    // 首尾的空白去掉；只剩标点或空白就不显示。
    const auto first = reply_so_far_.find_first_not_of(" \n\t");
    if (session_live_ && first != std::string::npos) {
        auto text = reply_so_far_.substr(first, reply_so_far_.find_last_not_of(" \n\t") - first + 1);
        captions_.push_back({false, text + "……"});
    }
    reply_so_far_.clear();
}
void ValetProtocol::ResyncPlayback() {
    played_base_ = Application::GetInstance().GetAudioService().PlayedPackets() - pushed_packets_;
    held_.clear(); held_samples_ = 0; holding_ = true;
}
bool ValetProtocol::HandReplyAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!on_incoming_audio_) return false;
    if (on_incoming_audio_(std::move(packet))) { ++pushed_packets_; return true; }
    if (!closing_) {
        interrupted_ = true;
        Application::GetInstance().GetAudioService().ResetDecoder(); ResyncPlayback();
        SetError("播放队列拥塞，请重新进入对话");
    }
    return false;
}
void ValetProtocol::PushReplyAudio(std::unique_ptr<AudioStreamPacket> packet) {
    constexpr size_t kPrerollSamples = 24000 * 300 / 1000;   // 300 ms
    // 只在扬声器真的空下来时（回答开头，或中途断过）才攒；只是追平网络时不攒，
    // 否则每追平一次就人为停 0.3 秒（2026-11-22 实测反而更卡）。
    if (!holding_ && Application::GetInstance().GetAudioService().IsPlaybackIdle()) holding_ = true;
    if (!holding_) { HandReplyAudio(std::move(packet)); return; }
    if (held_.empty()) held_since_ = esp_timer_get_time();
    held_samples_ += packet->payload.size() / 2;
    held_.push_back(std::move(packet));
    if (held_samples_ >= kPrerollSamples) ReleaseHeldAudio();
}
void ValetProtocol::ReleaseHeldAudio() {
    holding_ = false;
    auto held = std::move(held_);
    held_.clear(); held_samples_ = 0;
    for (auto& packet : held) if (!HandReplyAudio(std::move(packet))) break;   // 失败时已清空并报错
}
void ValetProtocol::DrainPlayback() {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (!tool_call_.empty() && esp_timer_get_time() > tool_deadline_) FinishTool("error", "处理超时（2 分钟没有结果）");
    const auto now = esp_timer_get_time();
    if (authenticated_ && now - last_ping_ > 20000000) { socket_->Ping(); last_ping_ = now; }
    // 打断后一直等不到网关「已停止输出」的回复：2 秒后不再等，否则之后的回答全被当成旧回答丢掉，通话像是没反应。
    if (interrupted_ && session_live_ && interrupt_at_ && now - interrupt_at_ > 2000000) {
        ESP_LOGW(TAG, "call: no reply to the interrupt after 2 s, listening again");
        interrupted_ = false; interrupt_at_ = 0; Event("tts", "stop");
    }
    if (live_ready_ && !call_ready_seen_) { call_ready_seen_ = true; call_retries_ = 0; call_retrying = false; }
    if (recreate_at_ && now >= recreate_at_ && authenticated_ && session_live_ && opened_) CreateCall();
    // 连接悄悄断了（没有关闭帧也没有 RST，比如隧道挂了）：每 20 秒一次的 ping 连着 2 次没回音就当断线。
    if (authenticated_ && session_live_ && opened_ && !reconnecting_ && socket_) {
        const int64_t heard = socket_->LastReceiveUs();
        if (heard && now - heard > 50000000) {
            ESP_LOGW(TAG, "call: nothing from the gateway for %d s", int((now - heard) / 1000000));
            authenticated_ = false; live_ready_ = false;
            StartReconnect();
            return;
        }
    }
    if (awaiting_ && now > ask_deadline_) { awaiting_ = false; SetError("助理回复超时，请重新提问"); }
    if (awaiting_ && !run_id_.empty() && now - last_poll_ > 3000000) {
        last_poll_ = now;
        auto* history = valet::wire::Request("valet-history", "chat.history"); auto* params = cJSON_GetObjectItem(history, "params");
        cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str()); cJSON_AddNumberToObject(params, "limit", 8);
        cJSON_AddNumberToObject(params, "maxBytes", 8192); Send(history);
        auto* transcript = valet::wire::Request("valet-transcript", "passport.transcript"); params = cJSON_GetObjectItem(transcript, "params");
        cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str()); cJSON_AddStringToObject(params, "runId", run_id_.c_str());
        cJSON_AddStringToObject(params, "requestId", request_id_.c_str()); Send(transcript);
    }
    // 播到哪个确认点就回哪个，不等扬声器完全空下来：助理连着说时扬声器一直不空。
    auto& audio = Application::GetInstance().GetAudioService();
    // 声音来得慢、攒不够时最多等 0.3 秒就先放出去。
    if (!held_.empty() && esp_timer_get_time() - held_since_ >= 300000) ReleaseHeldAudio();
    auto* engine = Engine();
    const bool idle = engine ? engine->PlaybackIdle() : audio.IsPlaybackIdle() && held_.empty();
    const uint64_t played = engine ? engine->Played() : audio.PlayedPackets() - played_base_;
    while (!marks_.empty() && !interrupted_ && (idle || played >= marks_.front().target)) {
        auto* root = valet::wire::Request("valet-mark", "talk.session.acknowledgeMark");
        auto* params = cJSON_GetObjectItem(root, "params");
        cJSON_AddStringToObject(params, "sessionId", call_id_.c_str());
        cJSON_AddStringToObject(params, "markName", marks_.front().name.c_str());
        Send(root); marks_.pop_front();
    }
    if (!idle) { quiet_since_ = 0; return; }
    const int64_t now_us = esp_timer_get_time();
    if (!quiet_since_) quiet_since_ = now_us;
    // 一段话播完，或扬声器已经安静 0.6 秒（网络慢时中途停顿）才把攒着的字幕上屏。
    if (!captions_.empty() && (audio_done_ || now_us - quiet_since_ >= 600000)) {
        for (const auto& caption : captions_)
            Event(caption.user ? "stt" : "tts", caption.user ? nullptr : "sentence_start", caption.text);
        captions_.clear();
    }
    if (audio_done_ && output_started_) {
        if (level_samples_) ESP_LOGI(TAG, "reply audio: %u ms, received peak %d rms %d, gain %.2f..%.2f",
                                     unsigned(level_samples_ / 24), level_peak_, int(sqrt(double(level_sumsq_) / level_samples_)),
                                     double(agc_gain_min_), double(agc_gain_max_));
        agc_gain_min_ = 99.f; agc_gain_max_ = 0.f;
        level_peak_ = 0; level_sumsq_ = level_samples_ = 0;
        audio_done_ = false; output_started_ = false; speech_active_ = false; Event("tts", "stop");
    }
}

void ValetProtocol::CompleteReply(const std::string& text) {
    if (reply_completed_) return;
    reply_completed_ = true;
    if (!text.empty()) Event("tts", "sentence_start", text);
    if (!session_live_ && speech_supported_ && chat_cloud::LocalTtsEnabled() && !text.empty()) {
        speech_active_ = true; speech_sequence_ = 0;
        auto* request = valet::wire::Request(("valet-speech-" + request_id_).c_str(), "valet.speech.start");
        auto* params = cJSON_GetObjectItem(request, "params");
        cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str());
        cJSON_AddStringToObject(params, "runId", run_id_.c_str());
        if (Send(request)) return;
        speech_active_ = false;
    }
    Event("tts", "stop");
}
