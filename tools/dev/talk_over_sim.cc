// Run the call talk-over detector (main/audio/talk_over.h) on a recording made with the device's
// voiceprobe dev command: mic.raw and ref.raw, 16 kHz s16 mono, same frame clock.
//   c++ -O2 -std=c++17 -I main/audio tools/dev/talk_over_sim.cc -o /tmp/talk_over_sim
//   /tmp/talk_over_sim mic.raw ref.raw [trace]
#include "talk_over.h"
#include <cstdio>
#include <vector>
static std::vector<int16_t> Load(const char* path) {
    std::vector<int16_t> v;
    if (FILE* f = std::fopen(path, "rb")) { int16_t b[4096]; size_t n; while ((n = std::fread(b, 2, 4096, f)) > 0) v.insert(v.end(), b, b + n); std::fclose(f); }
    return v;
}
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const auto mic = Load(argv[1]), ref = Load(argv[2]);
    TalkOver det;
    const size_t frames = std::min(mic.size(), ref.size()) / TalkOver::kFrame;
    int triggers = 0;
    for (size_t k = 0; k < frames; ++k) {
        if (det.Process(&mic[k * TalkOver::kFrame], &ref[k * TalkOver::kFrame])) {
            std::printf("talk-over at %.2f s\n", k * 0.02); ++triggers;
        }
        if (argc > 3 && k % 50 == 0) std::printf("  %.1f s: level %.1f floor %.1f over %.1f coupling %.1f\n", k * 0.02, det.LastLevel(), det.Floor(), det.LastOver(), det.Coupling());
    }
    std::printf("%d triggers in %.1f s\n", triggers, frames * 0.02);
}
