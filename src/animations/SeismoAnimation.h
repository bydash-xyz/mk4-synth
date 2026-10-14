#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// A seismograph: hits are quakes, held notes are tremors, beats are grid lines.
// - Paper scrolls left past the pen at 16 px per beat (tempo-driven), with a dotted grid line on
//   every beat and a denser one on the downbeat (metronome downbeat, or every 4th free-running beat).
// - Every note (every voice that strikes, so chords work) kicks the needle, alternating direction,
//   harder with velocity. The needle is a damped 5 Hz spring, so it rings down; the housing jolts.
// - Each held voice adds a tremor scaled by its envelope: low notes make slow, wide waves, high
//   notes a fine, fast buzz. A chord sums them into one messy trace.
// - The needle swings on a pendulum arm with a weight, like the real instrument.
class SeismoAnimation : public IAnimation {
public:
    static constexpr int PEN_X = 94;           // pen column (newest history sample)
    static constexpr int MID = 40;             // trace rest row
    static constexpr int HIST = 95;            // paper columns 0..94
    static constexpr float PX_PER_BEAT = 16.0f;

    SeismoAnimation() { reset(); }

    void init() override { reset(); }

    void reset() override {
        tracker_.reset();
        for (int i = 0; i < HIST; ++i) { hist_[i] = 0.0f; marks_[i] = 0; }
        for (int v = 0; v < 8; ++v) phases_[v] = 0.0f;
        d_ = 0.0f;
        w_ = 0.0f;
        tremor_ = 0.0f;
        acc_ = 0.0f;
        freeBeat_ = 0.0f;
        lastBeat_ = -1;
        pendingMark_ = 0;
        jolt_ = 0.0f;
        sign_ = 1;
        columns_ = 0;
    }

    void update(float dt, const AnimationContext& ctx) override {
        uint8_t hits[8];
        const int n = tracker_.update(ctx, hits);
        for (int i = 0; i < n; ++i) {
            sign_ = -sign_;
            w_ += sign_ * (KICK_BASE + KICK_VEL * ctx.lastVelocity / 127.0f);
            jolt_ = 0.25f * ctx.lastVelocity / 127.0f + 0.05f;
        }

        freeBeat_ += ctx.bpm / 60.0f * dt;
        if (freeBeat_ >= 4000.0f) freeBeat_ -= 4000.0f; // multiple of 4 so downbeats stay put
        const int beatInt = ctx.metronomeOn ? ctx.currentBeat : static_cast<int>(std::floor(freeBeat_));
        if (beatInt != lastBeat_) {
            pendingMark_ = (ctx.metronomeOn ? ctx.isDownbeat : beatInt % 4 == 0) ? 2 : 1;
            lastBeat_ = beatInt;
        }

        // Advance the paper one column at a time, simulating the needle between columns
        const float speed = ctx.bpm / 60.0f * PX_PER_BEAT;
        acc_ += speed * dt;
        while (acc_ >= 1.0f) {
            acc_ -= 1.0f;
            step(1.0f / speed, ctx);
            std::memmove(hist_, hist_ + 1, (HIST - 1) * sizeof(float));
            hist_[HIST - 1] = d_ + tremor_;
            std::memmove(marks_, marks_ + 1, HIST - 1);
            marks_[HIST - 1] = pendingMark_;
            pendingMark_ = 0;
            ++columns_;
        }
        jolt_ = jolt_ - dt > 0.0f ? jolt_ - dt : 0.0f;
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        // Paper edges, beat grid and the trace
        for (int x = 0; x < HIST; x += 2) {
            AnimDraw::pixel(u8g2, x, 18);
            AnimDraw::pixel(u8g2, x, 61);
        }
        for (int x = 0; x < HIST; ++x) {
            const uint8_t m = marks_[x];
            if (!m) continue;
            for (int y = 20; y < 61; y += m == 2 ? 2 : 4) AnimDraw::pixel(u8g2, x, y);
        }
        for (int x = 1; x < HIST; ++x) AnimDraw::line(u8g2, x - 1, traceY(hist_[x - 1]), x, traceY(hist_[x]));

        const int penY = traceY(d_ + tremor_);
        const int j = jolt_ > 0.0f ? (static_cast<int>(std::floor(jolt_ * 40.0f)) % 2 ? 1 : -1) : 0;
        // Paper roll, pendulum arm with its weight, and the housing
        AnimDraw::rect(u8g2, 97, 17, 5, 46);
        AnimDraw::vspan(u8g2, 99, 18, 61);
        AnimDraw::line(u8g2, PEN_X + 1, penY, 118 + j, MID);
        AnimDraw::disc(u8g2, 108 + j, rnd(MID + (penY - MID) * 0.45f), 3);
        AnimDraw::box(u8g2, PEN_X - 1, penY - 1, 3, 3);
        AnimDraw::rect(u8g2, 113 + j, 17, 15, 46);
        AnimDraw::circle(u8g2, 118 + j, MID, 2);
        AnimDraw::line(u8g2, 120 + j, 18, 118 + j, MID - 2);
    }

    const char* getName() const override { return "Seismograph"; }

    // State, exposed for tests
    float getDeflection() const { return d_; }               // spring (quake) part of the trace
    float getTrace() const { return d_ + tremor_; }          // current pen offset from MID
    float getHistory(int x) const { return hist_[x]; }       // x = 0 (oldest) .. HIST-1 (pen)
    uint8_t getMark(int x) const { return marks_[x]; }       // 0 none, 1 beat, 2 downbeat
    uint32_t getColumnsScrolled() const { return columns_; }
    float getJolt() const { return jolt_; }

private:
    static constexpr float SPRING_HZ = 5.0f;
    static constexpr float DAMPING = 0.12f;     // zeta
    static constexpr float MAX_SUBSTEP = 0.004f;
    static constexpr float KICK_BASE = 120.0f;  // needle velocity kick, px/s
    static constexpr float KICK_VEL = 600.0f;   // extra kick at full velocity
    static constexpr float CLAMP_D = 19.0f;

    static int rnd(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

    static int traceY(float offset) {
        const int y = MID + rnd(offset);
        return y < 19 ? 19 : (y > 60 ? 60 : y);
    }

    // Integrates the needle spring over h seconds and samples the tremor
    void step(float h, const AnimationContext& ctx) {
        const float om = AnimUtil::TWO_PI_F * SPRING_HZ;
        int n = static_cast<int>(std::ceil(h / MAX_SUBSTEP));
        if (n < 1) n = 1;
        const float hs = h / n;
        for (int i = 0; i < n; ++i) {
            w_ += (-om * om * d_ - 2.0f * DAMPING * om * w_) * hs;
            d_ = AnimUtil::clampf(d_ + w_ * hs, -CLAMP_D, CLAMP_D);
        }
        float tr = 0.0f;
        for (int v = 0; v < 8; ++v) {
            const float env = ctx.voiceEnvelopes[v];
            if (env < 0.02f) continue;
            const float k = AnimUtil::noteToX(ctx.voiceNotes[v], ctx, 0.0f) / 127.0f;
            phases_[v] += AnimUtil::TWO_PI_F * (3.0f + k * 14.0f) * h;
            if (phases_[v] >= AnimUtil::TWO_PI_F) phases_[v] -= AnimUtil::TWO_PI_F * std::floor(phases_[v] / AnimUtil::TWO_PI_F);
            tr += std::sin(phases_[v]) * env * (6.0f - k * 4.0f);
        }
        tremor_ = tr;
    }

    AnimUtil::StrikeTracker tracker_;
    float hist_[HIST];
    uint8_t marks_[HIST];
    float phases_[8];
    float d_ = 0.0f, w_ = 0.0f, tremor_ = 0.0f, acc_ = 0.0f, freeBeat_ = 0.0f, jolt_ = 0.0f;
    int lastBeat_ = -1;
    uint8_t pendingMark_ = 0;
    int sign_ = 1;
    uint32_t columns_ = 0;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
