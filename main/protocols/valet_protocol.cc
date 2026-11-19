#include "valet_protocol.h"
#include "valet_wire.h"
#include "valet_crypto/monocypher-ed25519.h"
#include "valet_crypto/passport_ima.h"
#include "board.h"
#include "display.h"
#include "settings.h"
#include "chat_cloud.h"
#include "application.h"
#include "audio/audio_service.h"
#include "assets/lang_config.h"
#include <esp_random.h>
#include <esp_log.h>
#include <psa/crypto.h>
#include <nvs.h>
#include <cstring>
#include <algorithm>
#include <memory>

namespace {
uint8_t secret[64]{}, public_key[32]{};
std::string device_id;
std::atomic<bool> live_mode{false}, muted{false};
constexpr EventBits_t kHello = BIT0, kFailed = BIT1, kInbox = BIT2, kExit = BIT3, kContext = BIT4, kSubscribed = BIT5;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using valet::wire::String;
}
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
ValetProtocol::~ValetProtocol() { CloseAudioChannel(); vEventGroupDelete(events_); }
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
    if (closing_ || !length) return;
    if (length > 32768) { SetError("网关消息超过设备限制"); xEventGroupSetBits(events_, kFailed); return; }
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    if (inbox_.size() >= 16) { xEventGroupSetBits(events_, kFailed); return; }
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
    socket_.reset(); // Receiver has stopped; join network callbacks before freeing this object.
    std::lock_guard<std::mutex> lock(inbox_mutex_); inbox_.clear();
}
bool ValetProtocol::Connect(bool bootstrap) {
    StopReceiver(); closing_ = false; authenticated_ = false; bootstrap_ = bootstrap;
    xEventGroupClearBits(events_, kHello | kFailed | kExit | kInbox | kContext | kSubscribed);
    socket_ = Board::GetInstance().GetNetwork()->CreateWebSocket(1);
    if (!socket_) return false;
    socket_->SetReceiveBufferSize(16384);
    socket_->OnData([this](const char* data, size_t count, bool binary) { if (!binary) Receive(data, count); });
    socket_->OnDisconnected([this] { authenticated_ = false; live_ready_ = false; xEventGroupSetBits(events_, kFailed); if (!closing_) SetError("助理连接已断开，请重新进入"); });
    if (xTaskCreate([](void* arg) { static_cast<ValetProtocol*>(arg)->RunReceiver(); }, "valet_receive", 10240, this, 4, &receiver_) != pdPASS) return false;
    const auto endpoint = valet::wire::Endpoint(profile_.connection.url);
    if (endpoint.empty() || !socket_->Connect(endpoint.c_str())) return false;
    const auto bits = xEventGroupWaitBits(events_, kHello | kFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & kHello) && !(bits & kFailed) && authenticated_;
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
        token_ = valet::wire::PairToken(profile_.connection.code);
        if (token_.empty() || !Connect(true)) { SetError("配对未完成，请在助理侧批准设备或重新生成配对码"); CloseAudioChannel(); return false; }
        auto updated = valet::LoadAssistantProfiles();
        bool found = false;
        for (auto& item : updated.items) if (item.id == profile_.id && item.connection.url == profile_.connection.url) {
            item.device_token = token_; item.connection.code.clear(); item.ask = true; found = true;
        }
        if (!found || !valet::SaveAssistantProfiles(updated)) { SetError("设备授权保存失败，请重新配对"); CloseAudioChannel(); return false; }
    }
    if (!Connect(false)) { SetError("网关签名认证失败，请检查插件版本与设备授权"); CloseAudioChannel(); return false; }
    if (context_.Valid() && session_live_ && !book_live_supported_) {
        SetError("网关未支持带书通话，请升级共享网关"); CloseAudioChannel(); return false;
    }
    if (context_.Valid() && !context_supported_) {
        SetError("网关未支持阅读上下文，请升级共享网关插件"); CloseAudioChannel(); return false;
    }
    if (context_supported_) {
        context_selected_ = false;
        auto* request = valet::wire::Request("valet-context", "valet.context.set");
        auto* params = cJSON_GetObjectItem(request, "params");
        cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str());
        cJSON_AddItemToObject(params, "context", valet::ContextJson(context_));
        if (!Send(request) || !(xEventGroupWaitBits(events_, kContext | kFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000)) & kContext) || !context_selected_) {
            SetError("阅读会话选择失败，问题未发送"); CloseAudioChannel(); return false;
        }
    }
    auto* subscribe = valet::wire::Request("valet-subscribe", "sessions.messages.subscribe");
    cJSON_AddStringToObject(cJSON_GetObjectItem(subscribe, "params"), "key", session_id_.c_str());
    if (!Send(subscribe) || !(xEventGroupWaitBits(events_, kSubscribed | kFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000)) & kSubscribed)) {
        SetError("会话订阅失败，问题未发送"); CloseAudioChannel(); return false;
    }
    input_decoder_ = std::make_unique<OpusDecoderWrapper>(16000, 1, 60);
    input_resampler_.Configure(16000, 24000);
    opened_ = true;
    if (on_audio_channel_opened_) on_audio_channel_opened_();
    if (on_connected_) on_connected_();
    return true;
}
void ValetProtocol::CloseAudioChannel() {
    const bool was_open = opened_.exchange(false);
    // Join before touching state owned by the receiver.
    StopReceiver();
    recording_ = false; live_ready_ = false; awaiting_ = false; speech_active_ = false; recording_data_.clear(); input_decoder_.reset(); call_id_.clear(); early_ready_.clear();
    marks_.clear(); playback_sequence_ = 0; output_started_ = false; audio_done_ = false; call_pending_ = false; tool_call_.clear();
    if (was_open && on_audio_channel_closed_) on_audio_channel_closed_();
}
bool ValetProtocol::IsAudioChannelOpened() const { return opened_ && authenticated_ && !error_occurred_; }
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
    interrupted_ = false; recording_data_.clear(); recording_data_.reserve(16000 * 30 + 58);
    if (input_decoder_) input_decoder_->ResetState();
    recording_ = true;
}
bool ValetProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (!IsAudioChannelOpened()) return false;
    if (session_live_ && (!live_ready_ || Muted())) return true;
    if (!session_live_ && !recording_) return true;
    std::vector<int16_t> pcm;
    if (!input_decoder_ || !input_decoder_->Decode(std::move(packet->payload), pcm)) return false;
    if (session_live_) {
        std::vector<int16_t> resampled(input_resampler_.GetOutputSamples(pcm.size()));
        input_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
        auto* root = valet::wire::Request(("valet-a-" + std::to_string(++sequence_)).c_str(), "talk.session.appendAudio");
        auto* params = cJSON_GetObjectItem(root, "params"); cJSON_AddStringToObject(params, "sessionId", call_id_.c_str());
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
    if (recording_data_.size() + pcm.size() > 16000 * 30) { recording_ = false; SetError("录音最多 30 秒，请松手后重新提问"); return false; }
    for (auto sample : pcm) recording_data_.push_back(valet::wire::MuLaw(sample));
    return true;
}
bool ValetProtocol::Submit() {
    if (recording_data_.empty()) { SetError("没有收到录音，请重试"); return false; }
    // Move the recording into its WAV envelope, then free it before serializing JSON.
    const auto audio = [this] {
        const auto wav = valet::wire::Wav(std::move(recording_data_));
        return valet::wire::Base64(wav.data(), wav.size());
    }();
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
void ValetProtocol::SendStopListening() { std::lock_guard<std::recursive_mutex> lock(state_mutex_); if (recording_.exchange(false)) { Submit(); recording_data_.clear(); } }
void ValetProtocol::CreateCall() {
    call_pending_ = true;
    auto* root = valet::wire::Request("valet-call", "talk.session.create"); auto* params = cJSON_GetObjectItem(root, "params");
    cJSON_AddStringToObject(params, "mode", "realtime"); cJSON_AddStringToObject(params, "transport", "gateway-relay");
    cJSON_AddStringToObject(params, "brain", "agent-consult"); cJSON_AddStringToObject(params, "language", std::strcmp(Lang::Current(), "zh-CN") == 0 ? "zh" : "en");
    cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str());
    Send(root);
}
void ValetProtocol::SendAbortSpeaking(AbortReason) {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    marks_.clear(); audio_done_ = false; output_started_ = false; tool_call_.clear();
    interrupted_ = true; awaiting_ = false;
    Application::GetInstance().GetAudioService().ResetDecoder();
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
        if (!cJSON_IsNumber(timestamp) || timestamp->valuedouble < 0 || !Send(valet::wire::Connect(secret, public_key, device_id, token_, bootstrap_, String(payload, "nonce"), timestamp->valuedouble))) xEventGroupSetBits(events_, kFailed);
        return;
    }
    if (id == "valet-connect") {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) { xEventGroupSetBits(events_, kFailed); return; }
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
    if (id == "valet-interrupt") { if (session_live_) interrupted_ = false; Event("tts", "stop"); return; }
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
    if (id == "valet-tool") {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) { SetError("助理工具调用失败"); tool_call_.clear(); return; }
        tool_run_ = String(payload, "runId"); tool_key_ = String(payload, "agentSessionKey"); return;
    }
    if (!tool_call_.empty() && event == "chat" && String(payload, "sessionKey") == tool_key_ && String(payload, "runId") == tool_run_) {
        const std::string status = String(payload, "state");
        if (status == "final" || status == "error" || status == "aborted") {
            auto* result = valet::wire::Request("valet-tool-result", "talk.session.submitToolResult");
            auto* params = cJSON_GetObjectItem(result, "params");
            cJSON_AddStringToObject(params, "sessionId", call_id_.c_str()); cJSON_AddStringToObject(params, "callId", tool_call_.c_str());
            auto* answer = cJSON_AddObjectToObject(params, "result");
            cJSON_AddStringToObject(answer, status == "final" ? "result" : "error", valet::wire::MessageText(cJSON_GetObjectItemCaseSensitive(payload, "message")).c_str());
            Send(result); tool_call_.clear();
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
    if (type == "error" || type == "close") { live_ready_ = false; SetError(type == "close" ? "通话已结束" : "实时服务不可用，请检查助理配置"); return; }
    if (interrupted_) return;
    const auto* playback = cJSON_GetObjectItemCaseSensitive(payload, "playbackSequence");
    if (cJSON_IsNumber(playback)) {
        if (playback->valuedouble <= playback_sequence_) return;
        playback_sequence_ = playback->valuedouble;
    }
    if (type == "mark") {
        const std::string name = String(payload, "markName");
        if (marks_.size() >= 16 || name.empty() || name.size() > 128) { SetError("实时播放确认超出限制"); return; }
        marks_.push_back(name); return;
    }
    if (type == "toolCall") {
        if (!tool_call_.empty()) { SetError("助理工具调用尚未结束"); return; }
        const std::string name = String(payload, "name");
        const auto* args = cJSON_GetObjectItemCaseSensitive(payload, "args");
        if (name != "openclaw_agent_consult" || !cJSON_IsObject(args)) { SetError("暂不支持此实时工具"); return; }
        tool_call_ = String(payload, "callId"); tool_deadline_ = esp_timer_get_time() + 120000000;
        auto* request = valet::wire::Request("valet-tool", "talk.client.toolCall");
        auto* params = cJSON_GetObjectItem(request, "params");
        cJSON_AddStringToObject(params, "sessionKey", session_id_.c_str()); cJSON_AddStringToObject(params, "relaySessionId", call_id_.c_str());
        cJSON_AddStringToObject(params, "callId", tool_call_.c_str()); cJSON_AddStringToObject(params, "name", name.c_str());
        cJSON_AddItemToObject(params, "args", cJSON_Duplicate(args, true)); Send(request); return;
    }
    if (type == "transcript" || type == "partial") {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "final"))) Event(!std::strcmp(String(payload, "role"), "user") ? "stt" : "tts", !std::strcmp(String(payload, "role"), "user") ? nullptr : "sentence_start", String(payload, "text"));
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
        if (!output_started_) { output_started_ = true; Event("tts", "start"); }
        audio_done_ = false;
        auto packet = std::make_unique<AudioStreamPacket>(); packet->sample_rate = 24000; packet->pcm = true;
        packet->payload.resize(pcm.size()*2); std::memcpy(packet->payload.data(), pcm.data(), packet->payload.size());
        if (on_incoming_audio_ && !on_incoming_audio_(std::move(packet)) && !closing_) {
            interrupted_ = true;
            Application::GetInstance().GetAudioService().ResetDecoder();
            SetError("播放队列拥塞，请重新进入对话");
        }
    } else if (type == "audioDone") audio_done_ = true;
    else if (type == "clear") {
        marks_.clear(); output_started_ = false; audio_done_ = false;
        Application::GetInstance().GetAudioService().ResetDecoder();
        Event("tts", "stop");
    }
}

void ValetProtocol::DrainPlayback() {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (!tool_call_.empty() && esp_timer_get_time() > tool_deadline_) {
        SetError("助理工具调用超时"); tool_call_.clear();
    }
    const auto now = esp_timer_get_time();
    if (authenticated_ && now - last_ping_ > 20000000) { socket_->Ping(); last_ping_ = now; }
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
    if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) return;
    while (!marks_.empty() && !interrupted_) {
        auto* root = valet::wire::Request("valet-mark", "talk.session.acknowledgeMark");
        auto* params = cJSON_GetObjectItem(root, "params");
        cJSON_AddStringToObject(params, "sessionId", call_id_.c_str());
        cJSON_AddStringToObject(params, "markName", marks_.front().c_str());
        Send(root); marks_.pop_front();
    }
    if (audio_done_ && output_started_) { audio_done_ = false; output_started_ = false; speech_active_ = false; Event("tts", "stop"); }
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
