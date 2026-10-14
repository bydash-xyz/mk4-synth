#pragma once

#include "Parameters.h"
#include <cstdint>
#include <vector>

namespace Synth {

// Band-limited wavetable bank for the wavetable engine (PPG Wave / Waldorf Microwave style).
//
// WAVETABLE_COUNT tables of FRAMES single-cycle frames each; the voice morphs between neighbouring
// frames. Every frame is stored at LEVELS mip levels holding at most MAX_HARMONICS[level] harmonics, and
// a voice reads the richest level whose top harmonic stays below Nyquist, so high notes do not alias.
// Built additively once at init() (about 1M sine-table reads) into one heap block of int16 samples:
// about 200 KB, which the ESP32-S3 allocator places in PSRAM. Frames are 8x oversampled against their
// richest level (63 harmonics in 512 samples) so the images of linear interpolation stay 50 dB down.
// Each frame has a guard sample (STRIDE) so interpolation never wraps.
class Wavetables {
public:
    static constexpr int SIZE   = 512; // samples per frame
    static constexpr int STRIDE = SIZE + 1;
    static constexpr int FRAMES = 8;
    static constexpr int LEVELS = 6;
    static constexpr int MAX_HARMONICS[LEVELS] = {63, 32, 16, 8, 4, 2};
    static constexpr float SAMPLE_SCALE = 1.0f / 32767.0f;

    static void init();
    static bool isReady() { return !s_data.empty(); }

    // First sample of a frame (STRIDE samples), table in [0, WAVETABLE_COUNT), level in [0, LEVELS)
    static const int16_t* frame(int table, int level, int frameIndex) {
        return &s_data[((static_cast<size_t>(table) * LEVELS + level) * FRAMES + frameIndex) * STRIDE];
    }

    // Richest mip level whose highest harmonic stays below Nyquist for a phase increment (cycles/sample)
    static int levelForIncrement(float inc) {
        for (int l = 0; l < LEVELS - 1; ++l) {
            if (static_cast<float>(MAX_HARMONICS[l]) * inc < 0.5f) return l;
        }
        return LEVELS - 1;
    }

    static const char* tableName(uint8_t table);

private:
    static std::vector<int16_t> s_data;
};

} // namespace Synth
