#include "voice_engine.h"

#include "audio_codec.h"
#include "power_policy.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "VoiceEngine"

bool VoiceEngine::Start(AudioCodec* codec) {
    if (task_) return true;
    codec_ = codec;
    if (!codec_ || !codec_->HardwareReady()) { ESP_LOGE(TAG, "codec not ready"); return false; }
    if (!play_) play_ = static_cast<int16_t*>(heap_caps_malloc(kPlayRing * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!mic_) mic_ = static_cast<int16_t*>(heap_caps_malloc(kMicRing * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!mic_ready_) mic_ready_ = xSemaphoreCreateBinary();
    if (!stopped_) stopped_ = xSemaphoreCreateBinary();
    if (!play_ || !mic_ || !mic_ready_ || !stopped_) { ESP_LOGE(TAG, "out of memory"); return false; }
    {
        std::lock_guard<std::mutex> lock(play_mutex_);
        play_read_ = play_count_ = 0; waiting_ = true; flush_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(mic_mutex_);
        mic_read_ = mic_count_ = 0;
    }
    talk_over_.Reset();
    stop_ = false;
    xSemaphoreTake(stopped_, 0);
    codec_->EnableInput(true);
    codec_->EnableOutput(true);
    // 会话期间功放一直开着：电源策略默认跟着界面状态开关功放，会吃掉句子开头。
    PowerPolicy::GetInstance().Acquire(PowerNeed::LocalPlayback);
    // 和原来的播放任务同级（低于墨水屏扫描供数），栈在内部内存。
    if (xTaskCreate([](void* arg) { static_cast<VoiceEngine*>(arg)->Run(); }, "voice_io", 6144, this,
                    configMAX_PRIORITIES - 2, &task_) != pdPASS) {
        task_ = nullptr;
        PowerPolicy::GetInstance().Release(PowerNeed::LocalPlayback);
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    ESP_LOGI(TAG, "started");
    return true;
}

void VoiceEngine::Stop() {
    if (!task_) return;
    stop_ = true;
    armed_ = false;
    // 读 I2S 最长阻塞 250 ms
    if (xSemaphoreTake(stopped_, pdMS_TO_TICKS(2000)) != pdTRUE) ESP_LOGW(TAG, "audio task did not stop in time");
    vTaskDelete(task_);
    task_ = nullptr;
    PowerPolicy::GetInstance().Release(PowerNeed::LocalPlayback);
    const auto s = TakeStats();
    ESP_LOGI(TAG, "stopped: %u frames, %u ran dry, %u overflows, %u mic dropped, worst loop %d ms",
             unsigned(s.frames), unsigned(s.underruns), unsigned(s.overflows), unsigned(s.mic_dropped), s.max_loop_ms);
}

void VoiceEngine::Play(const int16_t* pcm, size_t n) {
    if (!play_ || !n) return;
    std::lock_guard<std::mutex> lock(play_mutex_);
    if (play_count_ == 0 && waiting_) first_queued_us_ = esp_timer_get_time();
    for (size_t i = 0; i < n;) {
        if (play_count_ == kPlayRing) {   // 满了：丢最旧的
            const size_t drop = std::min(n - i, size_t(kFrame));
            play_read_ = (play_read_ + drop) % kPlayRing; play_count_ -= drop;
            overflows_.fetch_add(1);
        }
        const size_t at = (play_read_ + play_count_) % kPlayRing;
        const size_t chunk = std::min({n - i, kPlayRing - at, kPlayRing - play_count_});
        std::memcpy(play_ + at, pcm + i, chunk * sizeof(int16_t));
        play_count_ += chunk; i += chunk;
    }
    queued_.fetch_add(n);
}

void VoiceEngine::Flush() {
    std::lock_guard<std::mutex> lock(play_mutex_);
    if (play_count_) flush_ = true;
}

bool VoiceEngine::PlaybackIdle() const {
    auto& self = const_cast<VoiceEngine&>(*this);
    std::lock_guard<std::mutex> lock(self.play_mutex_);
    return play_count_ == 0 && !flush_;
}

void VoiceEngine::TakePlayback(int16_t* out) {
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(play_mutex_);
        if (flush_) {
            // 淡出 10 ms，其余丢掉
            constexpr size_t kFade = kRate / 100;
            n = std::min(play_count_, kFade);
            for (size_t i = 0; i < n; ++i)
                out[i] = int16_t(play_[(play_read_ + i) % kPlayRing] * float(kFade - i) / float(kFade));
            play_read_ = (play_read_ + play_count_) % kPlayRing; play_count_ = 0;
            flush_ = false; waiting_ = true;
            played_.fetch_add(n);
            std::fill(out + n, out + kFrame, 0);
            n = 0;   // 淡出段已经填好
        } else {
            // 播放环空了之后，先攒够 kPrebufferMs（或最多等 kPrebufferWaitMs）再出声：
            // 网关的声音按说话速度到达，不攒一点，网络稍慢就断音。
            if (waiting_ && play_count_ &&
                (play_count_ >= size_t(kRate) * kPrebufferMs / 1000 ||
                 esp_timer_get_time() - first_queued_us_ >= int64_t(kPrebufferWaitMs) * 1000))
                waiting_ = false;
            if (!waiting_) {
                n = std::min(play_count_, size_t(kFrame));
                for (size_t i = 0; i < n; ++i) out[i] = play_[(play_read_ + i) % kPlayRing];
                play_read_ = (play_read_ + n) % kPlayRing; play_count_ -= n;
                played_.fetch_add(n);
                if (play_count_ == 0) {   // 播空了：这一段说完，或者网络跟不上（两者分不开，只记次数）
                    waiting_ = true;
                    underruns_.fetch_add(1);
                }
            }
            std::fill(out + n, out + kFrame, 0);
        }
    }
    double sq = 0;
    for (int i = 0; i < kFrame; ++i) sq += double(out[i]) * out[i];
    const int rms = int(std::sqrt(sq / kFrame));
    const int64_t now = esp_timer_get_time();
    if (rms > 30) last_sound_us_.store(now);
    if (rms) { output_level_.store(rms); output_level_at_.store(now); }
}

void VoiceEngine::Run() {
    const int channels = codec_->input_channels();
    std::vector<int16_t> out(kFrame), in(size_t(kFrame) * channels);
    int16_t mono[kFrame];
    int64_t last = esp_timer_get_time();
    while (!stop_) {
        TakePlayback(out.data());
        codec_->OutputData(out);                    // TX 队列满时在这里等
        if (!codec_->InputData(in)) { vTaskDelay(1); continue; }
        const int64_t now = esp_timer_get_time();
        const int loop_ms = int((now - last) / 1000); last = now;
        if (loop_ms > max_loop_ms_.load()) max_loop_ms_.store(loop_ms);
        frames_.fetch_add(1);
        for (int i = 0; i < kFrame; ++i) mono[i] = in[size_t(i) * channels];
        // 一直跑（底噪、回声比例一直在学），只在通话里才算数
        if (talk_over_.Process(mono, out.data()) && armed_.load()) {
            talk_overs_.fetch_add(1);
            ESP_LOGI(TAG, "talk-over: mic band %.1f dB, floor %.1f, over echo %.1f dB, echo coupling %.1f dB",
                     double(talk_over_.LastLevel()), double(talk_over_.Floor()), double(talk_over_.LastOver()), double(talk_over_.Coupling()));
        }
        {
            std::lock_guard<std::mutex> lock(mic_mutex_);
            if (mic_count_ + kFrame > kMicRing) {   // 没人取：丢最旧的一帧
                mic_read_ = (mic_read_ + kFrame) % kMicRing; mic_count_ -= kFrame;
                mic_dropped_.fetch_add(1);
            }
            const size_t at = (mic_read_ + mic_count_) % kMicRing;
            for (int i = 0; i < kFrame; ++i) mic_[(at + i) % kMicRing] = mono[i];
            mic_count_ += kFrame;
            mic_at_us_ = now;
        }
        xSemaphoreGive(mic_ready_);
    }
    xSemaphoreGive(stopped_);
    vTaskSuspend(nullptr);
}

bool VoiceEngine::ReadMic(int16_t* frame, int timeout_ms, int64_t* at_us) {
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mic_mutex_);
            if (mic_ && mic_count_ >= size_t(kFrame)) {
                for (int i = 0; i < kFrame; ++i) frame[i] = mic_[(mic_read_ + i) % kMicRing];
                mic_read_ = (mic_read_ + kFrame) % kMicRing; mic_count_ -= kFrame;
                if (at_us) *at_us = mic_at_us_ - int64_t(mic_count_ / kFrame) * 20000;
                return true;
            }
        }
        const int64_t left = deadline - esp_timer_get_time();
        if (left <= 0 || !task_) return false;
        xSemaphoreTake(mic_ready_, pdMS_TO_TICKS(std::max<int64_t>(1, left / 1000)));
    }
}

void VoiceEngine::DropMic() {
    std::lock_guard<std::mutex> lock(mic_mutex_);
    mic_read_ = (mic_read_ + mic_count_) % kMicRing; mic_count_ = 0;
}

void VoiceEngine::ArmTalkOver(bool armed) { armed_.store(armed); }

VoiceEngine::Stats VoiceEngine::TakeStats() {
    return {underruns_.exchange(0), overflows_.exchange(0), mic_dropped_.exchange(0), frames_.exchange(0), max_loop_ms_.exchange(0)};
}
