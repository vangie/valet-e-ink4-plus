#pragma once

// 自建助理（提问 + 通话）用的语音引擎，设计见 docs/voice-engine.md。
// 一个音频任务每 20 ms 做一拍：从播放环取 320 点写给扬声器，再读 320 点麦克风。
// 写和读在同一拍里，所以录到的回声和播出的声音之间的延迟是固定的（实测 108 ms），插话检测靠它。
// 网络这边只往播放环里放、从上行环里取，两边互不阻塞。

#include "talk_over.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class AudioCodec;

class VoiceEngine {
public:
    static constexpr int kRate = 16000;
    static constexpr int kFrame = TalkOver::kFrame;      // 20 ms
    static constexpr int kPathDelayMs = 108;             // 扬声器 → 麦克风（实测）

    bool Start(AudioCodec* codec);
    void Stop();
    bool Running() const { return task_ != nullptr; }

    // ---- 播放（16 kHz 单声道）----
    // 放进播放环，不阻塞。播放环满了丢最旧的（记日志）。
    void Play(const int16_t* pcm, size_t n);
    // 清空还没播的：当前这一拍淡出 10 ms，下一拍开始静音。
    void Flush();
    // 累计放进来的 / 已经写给扬声器的样本数（只增不减；Flush 丢掉的不算已播）。
    uint64_t Queued() const { return queued_.load(); }
    uint64_t Played() const { return played_.load(); }
    // 没有待播的声音（播放环空，也不在等缓冲）。
    bool PlaybackIdle() const;
    // 最后一次扬声器出声的时刻（esp_timer 微秒）；从这一刻再过 kPathDelayMs 回声才进麦克风。
    int64_t LastSoundUs() const { return last_sound_us_.load(); }
    int OutputLevel(int64_t* at_us = nullptr) const { if (at_us) *at_us = output_level_at_.load(); return output_level_.load(); }

    // ---- 麦克风 ----
    // 取一帧（320 点）麦克风，最多等 timeout_ms；拿到返回 true，并给出这帧读完的时刻。
    bool ReadMic(int16_t* frame, int timeout_ms, int64_t* at_us = nullptr);
    // 丢掉还没取走的麦克风帧（开始新的录音前）。
    void DropMic();

    // ---- 插话检测 ----
    // 通话中、没静音时打开。每判定一次插话计数加一。
    void ArmTalkOver(bool armed);
    uint32_t TalkOvers() const { return talk_overs_.load(); }

    struct Stats { uint32_t underruns, overflows, mic_dropped, frames; int max_loop_ms; };
    Stats TakeStats();

private:
    static constexpr size_t kPlayRing = size_t(kRate) * 30;   // 30 秒
    static constexpr size_t kMicRing = size_t(kRate) * 2;     // 2 秒
    static constexpr int kPrebufferMs = 200, kPrebufferWaitMs = 300;

    void Run();
    void TakePlayback(int16_t* out);   // I/O 任务里取一拍要播的

    AudioCodec* codec_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stop_{false};
    SemaphoreHandle_t stopped_ = nullptr;

    // 播放环（PSRAM），mutex 只在拷贝时持有
    std::mutex play_mutex_;
    int16_t* play_ = nullptr;
    size_t play_read_ = 0, play_count_ = 0;
    bool waiting_ = true;        // 播放环空后重新攒够 kPrebufferMs 再出声
    int64_t first_queued_us_ = 0;
    bool flush_ = false;
    std::atomic<uint64_t> queued_{0}, played_{0};
    std::atomic<int64_t> last_sound_us_{0}, output_level_at_{0};
    std::atomic<int> output_level_{0};

    // 麦克风上行环
    std::mutex mic_mutex_;
    int16_t* mic_ = nullptr;
    size_t mic_read_ = 0, mic_count_ = 0;
    SemaphoreHandle_t mic_ready_ = nullptr;
    int64_t mic_at_us_ = 0;

    TalkOver talk_over_;
    std::atomic<bool> armed_{false};
    std::atomic<uint32_t> talk_overs_{0};

    std::atomic<uint32_t> underruns_{0}, overflows_{0}, mic_dropped_{0}, frames_{0};
    std::atomic<int> max_loop_ms_{0};
};
