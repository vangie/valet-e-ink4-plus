// 仅测试版：量扬声器 → 麦克风这一路的延迟稳不稳（设计通话回声消除之前的测量）。
// 在首页空闲时运行：同一个循环里交替「写 20 ms 给扬声器、读 20 ms 麦克风」，每 0.5 秒放一声 20 ms 扫频，
// 记下它写出去时的样本序号，再在录到的声音里找它出现的位置。两者之差就是这一路的延迟（样本数）。
// 中途可以故意让循环停一下（模拟墨水屏刷新时音频任务抢不到 CPU），看停顿之后延迟变不变。
#include "sdkconfig.h"
#if CONFIG_VALET_DEV_LINK
#include "application.h"
#include "board.h"
#include "power_policy.h"
#include <http.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <cmath>
#include <string>
#include <tuple>
#include <vector>

namespace {
constexpr const char* TAG = "AudioProbe";
constexpr int kRate = 16000, kFrame = 320, kChirp = 320, kEvery = 8000, kSeconds = 12;
constexpr int kTotal = kRate * kSeconds;
int s_stall_ms = 0, s_amplitude = 12000;
int s_owner = 0;

void Template(int16_t* out) {
    for (int i = 0; i < kChirp; ++i) {   // 500 → 3500 Hz 线性扫频，两头加窗
        const double t = double(i) / kRate, f0 = 500, f1 = 3500, T = double(kChirp) / kRate;
        const double phase = 2 * M_PI * (f0 * t + (f1 - f0) * t * t / (2 * T));
        const double window = 0.5 - 0.5 * std::cos(2 * M_PI * i / (kChirp - 1));
        out[i] = int16_t(s_amplitude * window * std::sin(phase));
    }
}

void Run(void*) {
    auto& app = Application::GetInstance();
    int16_t chirp[kChirp];
    Template(chirp);
    auto* mic = static_cast<int16_t*>(heap_caps_calloc(kTotal + kFrame, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    std::vector<int> marks;
    if (!mic || !app.AcquireLocalRecording(&s_owner)) {
        ESP_LOGW(TAG, "cannot take the audio (not idle on the home screen?)");
        heap_caps_free(mic);
        vTaskDelete(nullptr);
        return;
    }
    std::vector<int16_t> out(kFrame), in;
    int written = 0, read = 0, stalls = 0;
    int64_t worst_gap = 0, last = esp_timer_get_time();
    while (read < kTotal) {
        for (int i = 0; i < kFrame; ++i) {
            const int k = (written + i) % kEvery;
            out[i] = k < kChirp ? chirp[k] : 0;
            if (k == 0) marks.push_back(written + i);
        }
        if (!app.WriteLocalPlayback(&s_owner, out)) { ESP_LOGW(TAG, "write failed at %d", written); break; }
        written += kFrame;
        if (!app.ReadLocalRecording(&s_owner, in)) { ESP_LOGW(TAG, "read failed at %d", read); break; }
        std::copy(in.begin(), in.end(), mic + read);
        read += int(in.size());
        const int64_t now = esp_timer_get_time();
        worst_gap = std::max(worst_gap, now - last); last = now;
        if (s_stall_ms && read >= kTotal / 2 && !stalls) {   // 中途停一下
            ++stalls;
            ESP_LOGI(TAG, "stalling %d ms at sample %d", s_stall_ms, read);
            vTaskDelay(pdMS_TO_TICKS(s_stall_ms));
            last = esp_timer_get_time();
        }
    }
    app.ReleaseLocalPlayback(&s_owner);
    ESP_LOGI(TAG, "done: written %d read %d, worst loop gap %d ms", written, read, int(worst_gap / 1000));
    // 每一声在录音里的位置：在 [写出位置 - 1000, 写出位置 + 6000] 里找归一化相关最大的地方
    double tt = 0;
    for (int i = 0; i < kChirp; ++i) tt += double(chirp[i]) * chirp[i];
    for (int w : marks) {
        int best = -1; double best_r = 0;
        for (int d = -1000; d <= 6000; ++d) {
            const int m = w + d;
            if (m < 0 || m + kChirp > read) continue;
            double xy = 0, yy = 0;
            for (int i = 0; i < kChirp; ++i) { xy += double(mic[m + i]) * chirp[i]; yy += double(mic[m + i]) * mic[m + i]; }
            const double r = xy / std::sqrt(tt * yy + 1.0);
            if (r > best_r) { best_r = r; best = d; }
        }
        double peak = 0;
        if (best >= 0) for (int i = 0; i < kChirp; ++i) peak = std::max(peak, std::fabs(double(mic[w + best + i])));
        ESP_LOGI(TAG, "chirp at %6d: delay %5d samples (%.2f ms), r %.3f, mic peak %d", w, best, best / 16.0, best_r, int(peak));
        vTaskDelay(1);
    }
    heap_caps_free(mic);
    vTaskDelete(nullptr);
}
}  // namespace

void ValetAudioProbe(int stall_ms, int amplitude) {
    s_stall_ms = stall_ms;
    s_amplitude = amplitude > 0 ? amplitude : 12000;
    // 栈要在内部内存：打开音频会读 NVS，读 flash 期间外部 PSRAM 上的栈不可访问（第一次就因此断言重启）
    if (xTaskCreatePinnedToCore(Run, "audio_probe", 6144, nullptr, configMAX_PRIORITIES - 3, nullptr, 1) != pdPASS)
        ESP_LOGW(TAG, "task create failed");
}

// 第二个测量：从 Mac 下载一段 16 kHz 单声道 PCM（助理的声音），用同样的收放循环播出去并录音，
// 再把「麦克风录到的」和「实际写给扬声器的」两路原样传回 Mac，离线调回声消除。
namespace {
std::string s_base;
void RunVoice(void*) {
    auto& app = Application::GetInstance();
    Board::GetInstance().EnsureNetworkReady(20000);   // 空闲时 Wi-Fi 会被关掉
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    std::string play;
    if (http && http->Open("GET", s_base + "/play.raw") && http->GetStatusCode() == 200) {
        const size_t length = http->GetBodyLength();
        play.reserve(length);
        char chunk[2048];
        for (int got; play.size() < length && (got = http->Read(chunk, sizeof(chunk))) > 0;) play.append(chunk, size_t(got));
        ESP_LOGI(TAG, "voice: body %u of %u bytes", unsigned(play.size()), unsigned(length));
    }
    if (http) http->Close();
    const int n_play = int(play.size() / 2);
    const int total = n_play + kRate * 2;
    ESP_LOGI(TAG, "voice: downloaded %d samples", n_play);
    auto* mic = static_cast<int16_t*>(heap_caps_calloc(total + kFrame, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    auto* ref = static_cast<int16_t*>(heap_caps_calloc(total + kFrame, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!n_play || !mic || !ref || !app.AcquireLocalRecording(&s_owner)) {
        ESP_LOGW(TAG, "voice: cannot start");
        heap_caps_free(mic); heap_caps_free(ref); vTaskDelete(nullptr); return;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::LocalPlayback);   // 功放全程开着（否则会被间歇关掉）
    const auto* src = reinterpret_cast<const int16_t*>(play.data());
    std::vector<int16_t> out(kFrame), in;
    int written = 0, read = 0;
    int64_t worst = 0, last = esp_timer_get_time();
    while (read < total) {
        for (int i = 0; i < kFrame; ++i) out[i] = written + i < n_play ? src[written + i] : 0;
        std::copy(out.begin(), out.end(), ref + written);
        if (!app.WriteLocalPlayback(&s_owner, out)) break;
        written += kFrame;
        if (!app.ReadLocalRecording(&s_owner, in)) break;
        std::copy(in.begin(), in.end(), mic + read);
        read += int(in.size());
        const int64_t now = esp_timer_get_time(); worst = std::max(worst, now - last); last = now;
    }
    app.ReleaseLocalPlayback(&s_owner);
    PowerPolicy::GetInstance().Release(PowerNeed::LocalPlayback);
    ESP_LOGI(TAG, "voice: played %d recorded %d, worst gap %d ms", written, read, int(worst / 1000));
    Board::GetInstance().EnsureNetworkReady(20000);
    for (auto [name, buf, n] : {std::tuple{"mic", mic, read}, std::tuple{"ref", ref, written}}) {
        auto up = Board::GetInstance().GetNetwork()->CreateHttp(0);
        up->SetHeader("Content-Type", "application/octet-stream");
        up->SetContent(std::string(reinterpret_cast<const char*>(buf), size_t(n) * 2));
        const bool ok = up->Open("POST", s_base + "/upload?name=" + name) && up->GetStatusCode() == 200;
        up->Close();
        ESP_LOGI(TAG, "voice: uploaded %s (%d samples): %s", name, n, ok ? "ok" : "FAILED");
    }
    heap_caps_free(mic); heap_caps_free(ref);
    vTaskDelete(nullptr);
}
}  // namespace

void ValetVoiceProbe(const char* base) {
    s_base = base;
    if (xTaskCreatePinnedToCore(RunVoice, "voice_probe", 8192, nullptr, configMAX_PRIORITIES - 3, nullptr, 1) != pdPASS)
        ESP_LOGW(TAG, "task create failed");
}
#endif
