#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// Plinko: a ball machine. Notes drop balls through four rows of pegs into eight bins.
// - Each new note (per voice, from StrikeTracker, so a chord drops several balls at once) drops a
//   ball from a carriage riding the top rail, at that key's position across the screen.
// - Loud notes (velocity > 90) drop heavy balls (radius 2). Pegs flash when they're hit.
// - Balls pile up in eight bins, one 2 px layer each. The fifth ball rings "DING": the bin flashes
//   and then empties, so repeating a riff fills the same bins like a slot machine.
// - With the metronome on, the carriage bobs at the start of every beat.
class PlinkoAnimation : public IAnimation {
public:
    PlinkoAnimation() {
        // Four staggered peg rows, 12 px apart, offset 6 px on odd rows
        static constexpr int ROW_Y[PEG_ROWS] = {28, 34, 40, 46};
        int n = 0;
        for (int r = 0; r < PEG_ROWS; ++r) {
            for (int x = 8 + (r % 2) * 6; x < 124 && n < MAX_PEGS; x += 12) pegs_[n++] = {x, ROW_Y[r], 0.0f};
        }
        pegCount_ = n;
    }

    void init() override { reset(); }

    void reset() override {
        tracker_.reset();
        ballCount_ = 0;
        for (int i = 0; i < BINS; ++i) { bins_[i] = 0; ding_[i] = 0.0f; }
        pipeX_ = 64.0f;
        pipeTarget_ = 64.0f;
        puff_ = 0.0f;
        bob_ = 0;
        for (int i = 0; i < pegCount_; ++i) pegs_[i].f = 0.0f;
    }

    void update(float dt, const AnimationContext& ctx) override {
        uint8_t hits[8];
        const int n = tracker_.update(ctx, hits);
        for (int k = 0; k < n; ++k) {
            const float x = AnimUtil::noteToX(ctx.voiceNotes[hits[k]], ctx, 4.0f);
            const int r = ctx.lastVelocity > 90 ? 2 : 1;
            if (ballCount_ < MAX_BALLS) balls_[ballCount_++] = {x, 23.0f, rng_.range(-6.0f, 6.0f), 10.0f, r, -1};
            pipeTarget_ = x;
            puff_ = 0.1f;
        }
        pipeX_ += (pipeTarget_ - pipeX_) * std::fmin(1.0f, dt * 18.0f);
        puff_ = std::fmax(0.0f, puff_ - dt);
        bob_ = ctx.metronomeOn ? (ctx.beatPhase < 0.15f ? 1 : 0) : 0;
        for (int i = 0; i < pegCount_; ++i) pegs_[i].f = std::fmax(0.0f, pegs_[i].f - dt);
        for (int i = 0; i < BINS; ++i) {
            if (ding_[i] > 0.0f) {
                ding_[i] -= dt;
                if (ding_[i] <= 0.0f) bins_[i] = 0;
            }
        }

        const float sub = dt / SUBSTEPS;
        for (int s = 0; s < SUBSTEPS; ++s) {
            for (int bi = 0; bi < ballCount_; ++bi) stepBall(balls_[bi], sub);
            // Landed balls join their bin; the fifth one (unless the bin is already dinging) rings it
            int w = 0;
            for (int bi = 0; bi < ballCount_; ++bi) {
                const Ball& b = balls_[bi];
                if (b.done < 0) { balls_[w++] = b; continue; }
                if (ding_[b.done] <= 0.0f && ++bins_[b.done] >= BIN_FULL) ding_[b.done] = DING_TIME;
            }
            ballCount_ = w;
        }
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        // Rail and carriage
        AnimDraw::hspan(u8g2, 0, 127, 17);
        const int px = rnd(pipeX_), py = 18 + bob_;
        AnimDraw::rect(u8g2, px - 4, py, 9, 4);
        const int nz = puff_ > 0.0f ? 2 : 1;
        AnimDraw::hspan(u8g2, px - nz, px + nz, py + 4);
        for (int i = 0; i < pegCount_; ++i) {
            const Peg& p = pegs_[i];
            if (p.f > 0.0f) {
                AnimDraw::hspan(u8g2, p.x - 1, p.x + 1, p.y);
                AnimDraw::vspan(u8g2, p.x, p.y - 1, p.y + 1);
            } else {
                AnimDraw::pixel(u8g2, p.x, p.y);
            }
        }
        for (int i = 0; i < ballCount_; ++i) AnimDraw::disc(u8g2, rnd(balls_[i].x), rnd(balls_[i].y), balls_[i].r);
        // Bins
        for (int i = 0; i <= BINS; ++i) AnimDraw::vspan(u8g2, i * 16 < 127 ? i * 16 : 127, 50, 62);
        AnimDraw::hspan(u8g2, 0, 127, 62);
        for (int i = 0; i < BINS; ++i) {
            const int x0 = i * 16 + 2, x1 = i * 16 + 14;
            if (ding_[i] > 0.0f) {
                const bool on = static_cast<int>(std::floor(ding_[i] * 16.0f)) % 2 == 0; // flashes
                if (on) AnimDraw::box(u8g2, x0, 51, x1 - x0 + 1, 11);
                AnimDraw::tinyText(u8g2, i * 16 + 8 - AnimDraw::tinyTextWidth("DING") / 2, 43, "DING");
            } else {
                for (int k = 0; k < bins_[i]; ++k) AnimDraw::hspan(u8g2, x0, x1, 61 - k * 2);
            }
        }
    }

    const char* getName() const override { return "Plinko"; }

    // State, exposed for tests
    int getBallCount() const { return ballCount_; }
    int getBallRadius(int i) const { return balls_[i].r; }
    int getBinCount(int bin) const { return bins_[bin]; }
    bool isDinging(int bin) const { return ding_[bin] > 0.0f; }
    float getCarriageX() const { return pipeX_; }
    int getPegCount() const { return pegCount_; }
    static int binOf(float x) { return static_cast<int>(AnimUtil::clampf(std::floor(x / 16.0f), 0.0f, BINS - 1.0f)); }

    static constexpr int BINS = 8;
    static constexpr int BIN_FULL = 5;
    static constexpr int MAX_BALLS = 30;

private:
    static constexpr int PEG_ROWS = 4;
    static constexpr int MAX_PEGS = 40;        // 10 per row
    static constexpr int SUBSTEPS = 3;
    static constexpr float GRAVITY = 220.0f;   // px/s^2
    static constexpr float BOUNCE = 1.55f;     // 1 + restitution
    static constexpr float DING_TIME = 0.45f;  // s the full bin flashes before emptying
    static constexpr float DIVIDER_TOP = 50.0f;

    struct Peg { int x, y; float f; };
    struct Ball { float x, y, vx, vy; int r; int done; }; // done = bin it landed in, -1 while falling

    // JS Math.round (halves round toward +infinity)
    static int rnd(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

    void stepBall(Ball& b, float sub) {
        b.vy += GRAVITY * sub;
        b.x += b.vx * sub;
        b.y += b.vy * sub;
        const float minD = b.r + 1.2f;
        for (int i = 0; i < pegCount_; ++i) {
            Peg& p = pegs_[i];
            const float dx = b.x - p.x, dy = b.y - p.y;
            if (std::fabs(dx) >= minD || std::fabs(dy) >= minD) continue; // cheap reject
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < minD && d > 0.001f) {
                const float nx = dx / d, ny = dy / d;
                b.x = p.x + nx * minD;
                b.y = p.y + ny * minD;
                const float vn = b.vx * nx + b.vy * ny;
                if (vn < 0.0f) {
                    b.vx -= BOUNCE * vn * nx;
                    b.vy -= BOUNCE * vn * ny;
                    b.vx += rng_.range(-8.0f, 8.0f);
                    p.f = 0.15f;
                }
            }
        }
        // Side walls everywhere, bin dividers below y 50
        float lo = static_cast<float>(b.r), hi = 127.0f - b.r;
        if (b.y + b.r > DIVIDER_TOP) {
            const int bin = binOf(b.x);
            lo = bin * 16 + 1.0f + b.r;
            hi = bin * 16 + 15.0f - b.r;
        }
        if (b.x < lo) { b.x = lo; b.vx = std::fabs(b.vx) * 0.5f; }
        if (b.x > hi) { b.x = hi; b.vx = -std::fabs(b.vx) * 0.5f; }
        const int bin = binOf(b.x);
        if (b.y + b.r >= 61.0f - bins_[bin] * 2.0f) b.done = bin;
    }

    AnimUtil::StrikeTracker tracker_;
    AnimUtil::XorShift32 rng_;
    Peg pegs_[MAX_PEGS] = {};
    int pegCount_ = 0;
    Ball balls_[MAX_BALLS] = {};
    int ballCount_ = 0;
    int bins_[BINS] = {};
    float ding_[BINS] = {};
    float pipeX_ = 64.0f;
    float pipeTarget_ = 64.0f;
    float puff_ = 0.0f;
    int bob_ = 0;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
