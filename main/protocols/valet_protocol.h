#pragma once
#include "protocol.h"
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
    void RunReceiver();
    void StopReceiver();
    std::unique_ptr<OpusDecoderWrapper> input_decoder_;
    OpusResampler input_resampler_;
    std::vector<uint8_t> recording_data_;
    std::string token_, call_id_, early_ready_, run_id_, reply_;
    std::deque<std::string> marks_;
    std::string tool_call_, tool_run_, tool_key_;
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
