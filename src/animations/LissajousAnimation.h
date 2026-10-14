#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// Vintage oscilloscope in X/Y mode: the trace is a Lissajous figure built from the notes you hold.
// - One note: the same tone on both axes, 90 degrees apart, slowly drifting -> a rotating ellipse.
// - Several notes: sorted by pitch and dealt alternately to X and Y at their real frequency ratios,
//   so consonant intervals draw clean knots (a fifth is the classic 3:2) and dissonant ones tangle.
// - The figure shrinks with the voice envelopes, so releases fade out on the screen.
// - The chord or interval being played is named in the corner (e.g. "C MAJ", "P5").
class LissajousAnimation : public IAnimation {
public:
    void init() override { reset(); }

    void reset() override {
        drift_ = 0.0f;
        time_ = 0.0f;
        voiceCount_ = 0;
        label_[0] = '\0';
    }

    void update(float dtSeconds, const AnimationContext& ctx) override {
        drift_ += DRIFT_SPEED * dtSeconds;
        if (drift_ > kTwoPi) drift_ -= kTwoPi;
        time_ += dtSeconds;
        if (time_ > 1000.0f) time_ -= 1000.0f;

        // Collect sounding voices, sorted by pitch (insertion sort, at most 8)
        voiceCount_ = 0;
        for (int v = 0; v < 8; ++v) {
            if (ctx.voiceEnvelopes[v] < MIN_ENVELOPE || ctx.voiceNotes[v] == 0) continue;
            int i = voiceCount_++;
            while (i > 0 && notes_[i - 1] > ctx.voiceNotes[v]) {
                notes_[i] = notes_[i - 1];
                envs_[i] = envs_[i - 1];
                --i;
            }
            notes_[i] = ctx.voiceNotes[v];
            envs_[i] = ctx.voiceEnvelopes[v];
        }
        chordName(notes_, voiceCount_, label_, sizeof(label_));
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        renderGraticule(u8g2);
        if (voiceCount_ == 0) {
            renderIdleTrace(u8g2);
        } else {
            renderFigure(u8g2);
        }
        if (label_[0] != '\0') {
            u8g2.setFont(u8g2_font_5x7_tf);
            const int w = u8g2.getStrWidth(label_);
            u8g2.drawStr(SCREEN_WIDTH - w - 1, ANIM_REGION_BOTTOM, label_);
        }
    }

    const char* getName() const override { return "Lissajous"; }

    // Names the held notes: a chord quality for 3+ notes (any inversion), an interval for 2,
    // the note name for 1. Writes "" when nothing matches.
    static void chordName(const uint8_t* sortedNotes, int count, char* out, size_t outSize) {
        static const char* const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        out[0] = '\0';
        if (count <= 0) return;
        if (count == 1) {
            std::snprintf(out, outSize, "%s%d", NOTE_NAMES[sortedNotes[0] % 12], sortedNotes[0] / 12 - 1);
            return;
        }
        if (count == 2) {
            static const char* const INTERVALS[12] = {"P8", "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "m7", "M7"};
            const int semis = sortedNotes[1] - sortedNotes[0];
            std::snprintf(out, outSize, "%s", semis == 0 ? "UNI" : INTERVALS[semis % 12]);
            return;
        }
        uint16_t pcs = 0;
        for (int i = 0; i < count; ++i) pcs |= static_cast<uint16_t>(1u << (sortedNotes[i] % 12));
        struct Shape { uint16_t mask; const char* name; };
        static constexpr Shape SHAPES[] = {
            {(1 << 0) | (1 << 4) | (1 << 7) | (1 << 11), "MAJ7"},
            {(1 << 0) | (1 << 3) | (1 << 7) | (1 << 10), "MIN7"},
            {(1 << 0) | (1 << 4) | (1 << 7) | (1 << 10), "7"},
            {(1 << 0) | (1 << 4) | (1 << 7), "MAJ"},
            {(1 << 0) | (1 << 3) | (1 << 7), "MIN"},
            {(1 << 0) | (1 << 3) | (1 << 6), "DIM"},
            {(1 << 0) | (1 << 4) | (1 << 8), "AUG"},
            {(1 << 0) | (1 << 5) | (1 << 7), "SUS4"},
            {(1 << 0) | (1 << 2) | (1 << 7), "SUS2"},
        };
        // Try every held pitch class as the root (handles inversions); the bass note wins ties
        for (int i = 0; i < count; ++i) {
            const int root = sortedNotes[i] % 12;
            const uint16_t rotated = static_cast<uint16_t>(((pcs >> root) | (pcs << (12 - root))) & 0x0FFF);
            for (const Shape& s : SHAPES) {
                if (rotated == s.mask) {
                    std::snprintf(out, outSize, "%s %s", NOTE_NAMES[root], s.name);
                    return;
                }
            }
        }
    }

    // State, exposed for tests
    int getVoiceCount() const { return voiceCount_; }
    const char* getLabel() const { return label_; }

    static constexpr int CENTRE_X = SCREEN_WIDTH / 2;
    static constexpr int CENTRE_Y = (ANIM_REGION_TOP + ANIM_REGION_BOTTOM) / 2; // 39
    static constexpr float AMP_X = 40.0f;
    static constexpr float AMP_Y = 21.0f;

private:
    static constexpr float kTwoPi = 6.28318530718f;
    static constexpr float DRIFT_SPEED = 0.9f;     // rad / s: slow rotation, like detuned oscillators
    static constexpr float MIN_ENVELOPE = 0.02f;
    static constexpr int SAMPLES = 240;
    static constexpr float CYCLES = 4.0f;          // periods of the lowest note drawn per frame

    void renderGraticule(U8G2& u8g2) const {
        // Dotted centre cross and a sparse dot grid, like the etched scale on a scope tube
        for (int x = 0; x < SCREEN_WIDTH; x += 4) AnimDraw::pixel(u8g2, x, CENTRE_Y);
        for (int y = ANIM_REGION_TOP; y <= ANIM_REGION_BOTTOM; y += 4) AnimDraw::pixel(u8g2, CENTRE_X, y);
        for (int x = CENTRE_X % 16; x < SCREEN_WIDTH; x += 16) {
            for (int y = CENTRE_Y - 16; y <= CENTRE_Y + 16; y += 8) AnimDraw::pixel(u8g2, x, y);
        }
    }

    void renderIdleTrace(U8G2& u8g2) const {
        // Resting beam: a flat line with a faint ripple
        int px = 0;
        int py = CENTRE_Y;
        for (int x = 2; x < SCREEN_WIDTH; x += 2) {
            const int y = CENTRE_Y + static_cast<int>(std::lround(std::sin(x * 0.25f + time_ * 4.0f) * 0.7f));
            AnimDraw::line(u8g2, px, py, x, y);
            px = x;
            py = y;
        }
    }

    void renderFigure(U8G2& u8g2) const {
        // Frequency ratios relative to the lowest note; each axis is normalised to its envelope sum
        float ratio[8];
        float peakEnv = 0.0f;
        float xEnv = 0.0f, yEnv = 0.0f;
        for (int i = 0; i < voiceCount_; ++i) {
            ratio[i] = std::exp2((notes_[i] - notes_[0]) / 12.0f);
            if (envs_[i] > peakEnv) peakEnv = envs_[i];
            ((i & 1) ? yEnv : xEnv) += envs_[i];
        }
        const bool single = (voiceCount_ == 1);
        if (single) yEnv = xEnv;
        const float size = std::sqrt(peakEnv > 1.0f ? 1.0f : peakEnv);
        const float ax = AMP_X * size / xEnv;
        const float ay = AMP_Y * size / yEnv;

        int px = 0, py = 0;
        for (int s = 0; s <= SAMPLES; ++s) {
            const float t = kTwoPi * CYCLES * s / SAMPLES;
            float x = 0.0f, y = 0.0f;
            if (single) {
                x = envs_[0] * std::sin(t);
                y = envs_[0] * std::sin(t + kHalfPi + drift_);
            } else {
                for (int i = 0; i < voiceCount_; ++i) {
                    // Upper voices drift a little against the bass so the figure slowly turns
                    const float v = envs_[i] * std::sin(ratio[i] * t + drift_ * i);
                    if (i & 1) y += v; else x += v;
                }
            }
            const int sx = CENTRE_X + static_cast<int>(std::lround(x * ax));
            const int sy = CENTRE_Y - static_cast<int>(std::lround(y * ay));
            if (s > 0) AnimDraw::line(u8g2, px, py, sx, sy);
            px = sx;
            py = sy;
        }
    }

    static constexpr float kHalfPi = 1.57079632679f;

    float drift_ = 0.0f;
    float time_ = 0.0f;
    int voiceCount_ = 0;
    uint8_t notes_[8] = {};
    float envs_[8] = {};
    char label_[12] = {};
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
