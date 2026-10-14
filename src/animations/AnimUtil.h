#pragma once

#include "IAnimation.h"
#include "KeyboardLayout.h"
#include <cstdint>

namespace HardwareDriver {
namespace AnimUtil {

// Non-drawing helpers shared by the OLED animations (drawing lives in AnimDraw.h).

// Not PI / TWO_PI: Arduino #defines those
constexpr float PI_F = 3.14159265f;
constexpr float TWO_PI_F = 6.28318531f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Per-instance xorshift32 PRNG: cheap, allocation-free and deterministic, so tests can replay it
struct XorShift32 {
    uint32_t state = 0x9E3779B9u;

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
    float range(float lo, float hi) { return lo + uniform() * (hi - lo); }
};

// Position of a note along the MK4 keybed, 0 (leftmost key) .. 1 (rightmost), undoing the
// octave/transpose shift. Notes off the keybed pin to the nearest end.
inline float keyFraction(int note, const AnimationContext& ctx) {
    const int key = note - ctx.keybedNoteOffset - KeyboardLayout::BASE_NOTE;
    return clampf(static_cast<float>(key), 0.0f, KeyboardLayout::KEY_COUNT - 1.0f) /
           (KeyboardLayout::KEY_COUNT - 1.0f);
}

// Screen x for a note, so the keybed spans the full width (less a margin on each side)
inline float noteToX(int note, const AnimationContext& ctx, float margin = 6.0f) {
    return margin + keyFraction(note, ctx) * (static_cast<float>(SCREEN_WIDTH - 1) - 2.0f * margin);
}

// Sum of all voice envelopes (0..8): a cheap "how much is sounding" measure
inline float voiceEnergy(const AnimationContext& ctx) {
    float e = 0.0f;
    for (int v = 0; v < 8; ++v) e += ctx.voiceEnvelopes[v];
    return e;
}

// Detects each new note per voice from the envelope snapshot. Fires once when a voice's envelope
// starts rising (attack) or when a sounding voice switches to a different note (steal / legato).
// OledDisplayUI only passes onNoteOn the last note of each frame, which drops all but one note of
// a chord; the per-voice envelopes see every note.
// The first update after reset() only records the voices, so notes still ringing when an
// animation is switched to don't register as new strikes.
class StrikeTracker {
public:
    StrikeTracker() { reset(); }

    void reset() {
        for (int v = 0; v < 8; ++v) {
            prev_[v] = 0.0f;
            prevNote_[v] = 0;
            armed_[v] = true;
        }
        primed_ = false;
    }

    // Writes the voice slots (0..7) that struck this frame into out; returns how many.
    int update(const AnimationContext& ctx, uint8_t out[8]) {
        int n = 0;
        const bool report = primed_;
        primed_ = true;
        for (int v = 0; v < 8; ++v) {
            const float e = ctx.voiceEnvelopes[v];
            const uint8_t note = ctx.voiceNotes[v];
            const bool rising = e > prev_[v] + 0.001f;
            const bool newNote = e > 0.01f && note != prevNote_[v] && prev_[v] > 0.01f;
            if ((rising && armed_[v]) || newNote) {
                if (report) out[n++] = static_cast<uint8_t>(v);
                armed_[v] = false;
            }
            if (e < prev_[v] - 0.0005f || e < 0.01f) armed_[v] = true;
            prev_[v] = e;
            if (e > 0.01f) prevNote_[v] = note;
        }
        return n;
    }

private:
    float prev_[8];
    uint8_t prevNote_[8];
    bool armed_[8];
    bool primed_ = false;
};

} // namespace AnimUtil
} // namespace HardwareDriver
