#include "image_tone.h"

#include <algorithm>
#include <cmath>

namespace reader {

namespace {
constexpr unsigned kLiftKnee = 128;      // 以上不动
constexpr double kLiftGamma = 0.72;
struct LiftTable {
    uint8_t v[256];
    LiftTable() {
        for (unsigned i = 0; i < 256; ++i) {
            double out = i <= kLiftKnee
                ? kLiftKnee * std::pow(double(i) / kLiftKnee, kLiftGamma)
                : double(i);
            v[i] = static_cast<uint8_t>(std::min(255.0, out + 0.5));
        }
    }
};
} // namespace

uint8_t LiftCoverPixel(uint8_t value) {
    static const LiftTable table;
    return table.v[value];
}

void ApplyImageDarkening(uint8_t* pixels, size_t size) {
    if (!pixels) return;
    for (size_t i = 0; i < size; ++i) {
        pixels[i] = DarkenImagePixel(pixels[i]);
    }
}

} // namespace reader
