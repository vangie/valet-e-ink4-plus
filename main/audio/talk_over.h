#pragma once

// 通话插话检测：助理说话时，判断麦克风里是不是另外有人在说话。
//
// 这块板子的回声路径（2026-11-23 在设备上实测，录音 + 离线分析）：
// - 扬声器 → 麦克风延迟固定 108 ms（同一个 I2S 时钟下收放），但音频芯片偶尔整块丢/补 5 ms，
//   延迟会跳到 93～108 ms 之间的另一个值；
// - 回声强烈非线性：1.5～6 kHz 比播出的声音还响 8～11 dB（扬声器失真），线性回声消除最多压 5 dB；
// - 1 kHz 以下回声很弱（比播出声低 11～16 dB，小扬声器出不了低频），而人说话的能量主要就在这里。
// 所以只看 150～1000 Hz：麦克风这一段比「由播出声预测的回声」高出很多、又明显高于底噪，就是有人说话。
// 判定：最近 15 帧（300 ms）里至少 9 帧比预测回声高 14 dB 以上、比底噪高 15 dB 以上。
// 离线（tools/dev/talk_over_sim.cc，两段各 30 秒纯回声 + 一段插话）：0 次误报，插话 0.1～0.25 秒内发现；
// 8～11 帧、12～15 dB 之间结果都一样。
//
// 不依赖 ESP-IDF，可以在电脑上用录音直接跑（tools/dev/talk_over_sim.cc）。

#include <cmath>
#include <cstddef>
#include <cstdint>

class TalkOver {
public:
    static constexpr int kFrame = 320;          // 20 ms @ 16 kHz
    static constexpr int kHistory = 8;          // 记最近 8 帧播出声（160 ms）
    static constexpr int kWindow = 15, kHits = 9;
    static constexpr float kOverEchoDb = 14.f, kOverFloorDb = 15.f;

    void Reset() { *this = TalkOver(); }

    // 每 20 ms 调一次：mic 是这一帧录到的，ref 是同一次循环写给扬声器的。返回 true = 判定有人插话。
    bool Process(const int16_t* mic, const int16_t* ref) {
        const float em = BandDb(mic_bp_, mic), er = BandDb(ref_bp_, ref);
        hist_[pos_ % kHistory] = er; ++pos_;
        // 回声 108 ms（≈5.4 帧）后到，芯片丢/补块时在 93～108 ms 间跳：取 3～7 帧前播出声的最大值
        float pred = -1e9f;
        for (int k = 3; k <= 7; ++k) if (pos_ > uint32_t(k)) pred = std::fmax(pred, hist_[(pos_ - 1 - k) % kHistory]);
        const bool playing = pred > kSilentDb;
        // 底噪：低于它马上跟下去；扬声器有声时不往上涨（否则回声本身会被当成底噪）
        if (!floor_known_ || em < floor_) floor_ = em;
        else if (!playing) floor_ += (em - floor_) * 0.01f;
        floor_known_ = true;
        const float over = em - pred - coupling_;
        // 回声比例按中位数跟（每帧挪 0.05 dB）：只在「不像人声」的帧上学会一直偏低（2026-11-23 学成 -22 dB，
        // 实际中位数 -16.5 dB），等于凭空多出 5 dB 人声、造成误报。
        if (playing) coupling_ += (em - pred > coupling_) ? 0.05f : -0.05f;
        const bool hit = playing && over > kOverEchoDb && em > floor_ + kOverFloorDb;
        window_ = ((window_ << 1) | (hit ? 1u : 0u)) & ((1u << kWindow) - 1);
        last_over_ = over; last_level_ = em;
        if (cooldown_) { --cooldown_; return false; }
        if (__builtin_popcount(window_) >= kHits) { window_ = 0; cooldown_ = 50; return true; }
        return false;
    }
    float LastOver() const { return last_over_; }
    float LastLevel() const { return last_level_; }
    float Floor() const { return floor_; }
    float Coupling() const { return coupling_; }

private:
    // 150 Hz 高通 + 1000 Hz 低通（二阶，Q=0.707），16 kHz
    struct Biquad { float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
        float Run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    struct BandPass {
        Biquad hp{0.95920f, -1.91841f, 0.95920f, -1.91674f, 0.92007f};
        Biquad lp{0.02995f, 0.05991f, 0.02995f, -1.45424f, 0.57406f};
    };
    static constexpr float kSilentDb = 40.f;   // 播出声这一段低于它就当扬声器没声
    static float BandDb(BandPass& bp, const int16_t* x) {
        double e = 0;
        for (int i = 0; i < kFrame; ++i) { const float y = bp.lp.Run(bp.hp.Run(float(x[i]))); e += double(y) * y; }
        return float(10.0 * std::log10(e + 1.0));
    }
    BandPass mic_bp_, ref_bp_;
    float hist_[kHistory] = {};
    uint32_t pos_ = 0, window_ = 0;
    int cooldown_ = 0;
    float floor_ = 0.f, coupling_ = -16.f, last_over_ = 0.f, last_level_ = 0.f;
    bool floor_known_ = false;
};
