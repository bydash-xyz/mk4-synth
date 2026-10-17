#pragma once

#include <cmath>

namespace Synth {

constexpr int SINE_LUT_SIZE = 2048;
constexpr float TWO_PI_F = 6.28318530717958647692f;

class SineLut {
public:
    static void init();

    // Largest |phase| (radians) lookupFast() accepts: keeps the table position inside int range
    static constexpr float kMaxFastPhase = 1.0e6f;

    // Interpolated sine lookup for phase in radians, safe for any input (NaN / inf / huge -> 0).
    // Use for phases derived from untrusted values; the audio path uses lookupFast().
    // (Builds with -ffast-math, like the firmware, may drop the NaN part of this check.)
    static inline float lookup(float phase) {
        if (!(std::fabs(phase) <= kMaxFastPhase)) return 0.0f; // also false for NaN
        return lookupFast(phase);
    }

    // Audio-path lookup. Precondition: phase is finite and |phase| <= kMaxFastPhase (true for the
    // engine's LFO / tremolo / metronome phases in [0, 2pi) and for FM phases built from bounded
    // preset parameters). Floors with a truncate-and-correct instead of std::floor, which is a
    // library call on the ESP32-S3; the result is identical to floor() in that range.
    static inline float lookupFast(float phase) {
        float pos = phase * (static_cast<float>(SINE_LUT_SIZE) / TWO_PI_F);
        int idx0 = static_cast<int>(pos);
        if (static_cast<float>(idx0) > pos) --idx0; // truncation rounds negatives up
        float frac = pos - static_cast<float>(idx0);
        int idx1 = idx0 & (SINE_LUT_SIZE - 1);
        int idx2 = (idx1 + 1) & (SINE_LUT_SIZE - 1);

        return s_table[idx1] + frac * (s_table[idx2] - s_table[idx1]);
    }

    static inline float lookupPhase32(uint32_t phase) {
        uint32_t idx0 = (phase >> 21) & (SINE_LUT_SIZE - 1);
        uint32_t idx1 = (idx0 + 1) & (SINE_LUT_SIZE - 1);
        float frac = static_cast<float>(phase & 0x1FFFFFu) * (1.0f / 2097152.0f);
        return s_table[idx0] + frac * (s_table[idx1] - s_table[idx0]);
    }

private:
    static float s_table[SINE_LUT_SIZE];
    static bool s_initialized;
};

} // namespace Synth
