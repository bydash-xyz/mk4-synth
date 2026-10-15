#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// Choir: eight blob singers standing on a floor line, one per synth voice slot, so a held chord
// shows its voicing.
// - Every new note (per voice, from StrikeTracker, so chords work) makes that voice's singer hop;
//   louder velocity hops higher. It also sends a little note glyph floating up (max 12).
// - The mouth opening follows the voice's envelope (slow pad attack = slow yawn, pluck = quick "pa").
//   Arms go up while the envelope is above 0.55.
// - Higher notes make the singer stand taller (stretch 0..10 px over notes 36..84).
// - With the metronome on, sounding singers sway on alternate beats.
// - After 6 s with no voice sounding they sag, close their eyes and float "z"s.
class ChoirAnimation : public IAnimation {
public:
    void init() override { reset(); }

    void reset() override {
        tracker_.reset();
        for (int v = 0; v < VOICES; ++v) {
            Singer& s = singers_[v];
            s.hop = 0.0f; s.vy = 0.0f; s.stretch = 0.0f; s.mouth = 0.0f;
            s.blink = rng_.range(1.0f, 4.0f); s.surprise = 0.0f; s.arms = 0.0f;
        }
        noteCount_ = 0;
        zCount_ = 0;
        quiet_ = 0.0f;
        zTimer_ = 0.0f;
    }

    void update(float dt, const AnimationContext& ctx) override {
        uint8_t hits[8];
        const int n = tracker_.update(ctx, hits);
        for (int k = 0; k < n; ++k) {
            Singer& s = singers_[hits[k]];
            if (s.hop >= -2.0f) {
                s.vy = -(HOP_BASE + HOP_VEL * ctx.lastVelocity / 127.0f);
            }
            s.surprise = 0.2f;
            if (noteCount_ < MAX_NOTES) {
                notes_[noteCount_++] = {static_cast<float>(slotX(hits[k]) + 4), 60.0f - 14.0f - s.stretch,
                                        rng_.range(0.0f, 6.0f)};
            }
        }

        bool any = false;
        for (int v = 0; v < VOICES; ++v) {
            Singer& s = singers_[v];
            const float env = ctx.voiceEnvelopes[v];
            const bool singing = env > SING_THRESHOLD;
            any = any || singing;
            const float target = singing
                ? AnimUtil::clampf((ctx.voiceNotes[v] - 36) / 48.0f, 0.0f, 1.0f) * MAX_STRETCH
                : (quiet_ > SLEEP_AFTER ? SLEEP_SAG : 0.0f);
            s.stretch += (target - s.stretch) * std::fmin(1.0f, dt * 12.0f);
            s.mouth += (env - s.mouth) * std::fmin(1.0f, dt * 20.0f);
            s.arms += ((env > 0.55f ? 1.0f : 0.0f) - s.arms) * std::fmin(1.0f, dt * 10.0f);
            s.hop += s.vy * dt;
            s.vy += GRAVITY * dt;
            s.hop = std::fmax(s.hop, -MAX_HOP);
            if (s.hop > 0.0f) { s.hop = 0.0f; s.vy = 0.0f; }
            s.surprise = std::fmax(0.0f, s.surprise - dt);
            s.blink -= dt;
            if (s.blink < -0.12f) s.blink = rng_.range(2.0f, 5.0f);
        }
        quiet_ = any ? 0.0f : quiet_ + dt;

        // Floating note glyphs rise and vanish above the header
        int w = 0;
        for (int i = 0; i < noteCount_; ++i) {
            FloatGlyph g = notes_[i];
            g.y -= 14.0f * dt;
            g.t += dt;
            if (g.y > 10.0f) notes_[w++] = g;
        }
        noteCount_ = w;

        if (quiet_ > SLEEP_AFTER) {
            zTimer_ -= dt;
            if (zTimer_ <= 0.0f && zCount_ < MAX_ZS) {
                zTimer_ = 1.3f;
                const int v = static_cast<int>(rng_.range(0.0f, 8.0f));
                zs_[zCount_++] = {static_cast<float>(slotX(v) + 4), 44.0f, 0.0f};
            }
        }
        w = 0;
        for (int i = 0; i < zCount_; ++i) {
            FloatGlyph z = zs_[i];
            z.y -= 8.0f * dt;
            z.t += dt;
            if (z.y > 12.0f && quiet_ > SLEEP_AFTER) zs_[w++] = z;
        }
        zCount_ = w;
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        AnimDraw::hspan(u8g2, 0, 127, 62);
        const int sway = (ctx.metronomeOn && ctx.activeVoiceCount > 0) ? ((ctx.currentBeat % 2) ? 1 : -1) : 0;
        const bool asleep = isAsleep();
        for (int v = 0; v < VOICES; ++v) {
            const Singer& s = singers_[v];
            const bool singing = ctx.voiceEnvelopes[v] > SING_THRESHOLD;
            const int cx = slotX(v) + (singing ? sway : 0);
            const int bh = rnd(14.0f + s.stretch);
            const int bot = 59 + rnd(s.hop);
            const int top = bot - bh + 1;
            // Bean body with clipped corners
            AnimDraw::hspan(u8g2, cx - 4, cx + 3, top);
            AnimDraw::hspan(u8g2, cx - 4, cx + 3, bot);
            AnimDraw::pixel(u8g2, cx - 5, top + 1); AnimDraw::pixel(u8g2, cx + 4, top + 1);
            AnimDraw::pixel(u8g2, cx - 5, bot - 1); AnimDraw::pixel(u8g2, cx + 4, bot - 1);
            AnimDraw::vspan(u8g2, cx - 6, top + 2, bot - 2);
            AnimDraw::vspan(u8g2, cx + 5, top + 2, bot - 2);
            // Feet (leave the floor when hopping)
            AnimDraw::vspan(u8g2, cx - 3, bot + 1, bot + 2);
            AnimDraw::vspan(u8g2, cx + 2, bot + 1, bot + 2);
            // Arms: down at rest, up when belting
            const int ay = top + rnd(bh * 0.55f);
            if (s.arms > 0.5f) {
                AnimDraw::line(u8g2, cx - 7, ay, cx - 9, ay - 3);
                AnimDraw::line(u8g2, cx + 6, ay, cx + 8, ay - 3);
            } else {
                AnimDraw::line(u8g2, cx - 7, ay, cx - 8, ay + 2);
                AnimDraw::line(u8g2, cx + 6, ay, cx + 7, ay + 2);
            }
            // Eyes
            const int ey = top + 4;
            if (asleep || s.blink < 0.0f) {
                AnimDraw::hspan(u8g2, cx - 4, cx - 3, ey + 1);
                AnimDraw::hspan(u8g2, cx + 2, cx + 3, ey + 1);
            } else if (s.surprise > 0.0f) {
                AnimDraw::box(u8g2, cx - 4, ey, 2, 2);
                AnimDraw::box(u8g2, cx + 2, ey, 2, 2);
            } else {
                AnimDraw::vspan(u8g2, cx - 3, ey, ey + 1);
                AnimDraw::vspan(u8g2, cx + 2, ey, ey + 1);
            }
            // Mouth opens with the envelope. (JS centres the ellipse at cx - 0.5, which rounds back
            // to cx for every span, so the integer centre draws the same pixels.)
            const int room = bot - 2 - (top + 8);
            int ry = rnd(s.mouth * (2.0f + s.stretch / 3.0f));
            const int maxRy = static_cast<int>(std::floor(room / 2.0f));
            if (maxRy < ry) ry = maxRy;
            if (singing && ry >= 1) AnimDraw::ellipseFill(u8g2, cx, top + 8 + ry, 2, ry);
            else AnimDraw::hspan(u8g2, cx - 2, cx + 1, top + 9);
        }
        static const char* const NOTE_GLYPH[5] = {"..#.", "..##", "..#.", "###.", "###."};
        for (int i = 0; i < noteCount_; ++i) {
            const FloatGlyph& g = notes_[i];
            AnimDraw::sprite(u8g2, rnd(g.x) + rnd(std::sin(g.t * 5.0f)), rnd(g.y), NOTE_GLYPH, 5);
        }
        for (int i = 0; i < zCount_; ++i) {
            const FloatGlyph& z = zs_[i];
            AnimDraw::tinyText(u8g2, rnd(z.x) + rnd(z.t * 3.0f), rnd(z.y), "z");
        }
    }

    const char* getName() const override { return "Choir"; }

    // State, exposed for tests
    float getHop(int v) const { return singers_[v].hop; }
    float getStretch(int v) const { return singers_[v].stretch; }
    float getMouth(int v) const { return singers_[v].mouth; }
    float getArms(int v) const { return singers_[v].arms; }
    bool isAsleep() const { return quiet_ > SLEEP_AFTER; }
    int getNoteGlyphCount() const { return noteCount_; }
    int getZCount() const { return zCount_; }
    static int slotX(int v) { return 8 + v * 16; }

    static constexpr int MAX_NOTES = 12;
    static constexpr float SLEEP_AFTER = 6.0f; // s of silence before the choir dozes off
    static constexpr float MAX_HOP = 26.0f;

private:
    static constexpr int VOICES = 8;
    static constexpr int MAX_ZS = 8;              // JS is unbounded; ~4 live at once in practice
    static constexpr float SING_THRESHOLD = 0.02f;
    static constexpr float MAX_STRETCH = 10.0f;
    static constexpr float SLEEP_SAG = -3.0f;
    static constexpr float HOP_BASE = 70.0f;      // px/s launch speed
    static constexpr float HOP_VEL = 90.0f;       // extra px/s at full velocity
    static constexpr float GRAVITY = 520.0f;      // px/s^2

    struct Singer { float hop, vy, stretch, mouth, blink, surprise, arms; };
    struct FloatGlyph { float x, y, t; };

    // JS Math.round (halves round toward +infinity)
    static int rnd(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

    AnimUtil::StrikeTracker tracker_;
    AnimUtil::XorShift32 rng_;
    Singer singers_[VOICES] = {};
    FloatGlyph notes_[MAX_NOTES] = {};
    FloatGlyph zs_[MAX_ZS] = {};
    int noteCount_ = 0;
    int zCount_ = 0;
    float quiet_ = 0.0f;
    float zTimer_ = 0.0f;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
