#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// Pond: a jelly-water surface you poke with notes, and a duck that rides the waves.
// - The surface is a 65-node spring mesh (one node per 2 px). Each new note (per voice, from
//   StrikeTracker, so chords work) pokes it at that key's spot across the screen (low keys left,
//   high keys right), harder with velocity, and throws up droplets that splash back down.
// - Held voices keep shimmering at their spot, faster for higher notes, so a held chord makes
//   standing ripples.
// - The duck paddles toward wherever you last played and rides the waves. A loud note right
//   under it (or a surface rising fast under it) launches it; the hardest hits make it backflip.
// - With the metronome on it quacks on every downbeat. After 6 s of silence it dozes off.
class PondAnimation : public IAnimation {
public:
    static constexpr int N = 65;       // mesh nodes, 2 px apart
    static constexpr float REST = 46.0f; // resting water line (screen y)

    void init() override { reset(); }

    void reset() override {
        tracker_.reset();
        for (int i = 0; i < N; ++i) { h_[i] = 0.0f; v_[i] = 0.0f; }
        dropCount_ = 0;
        zCount_ = 0;
        t_ = 0.0f;
        quiet_ = 0.0f;
        quack_ = 0.0f;
        zTimer_ = 0.0f;
        duck_ = Duck{};
    }

    void update(float dt, const AnimationContext& ctx) override {
        t_ += dt;
        uint8_t hits[8];
        const int n = tracker_.update(ctx, hits);
        for (int k = 0; k < n; ++k) {
            const float x = AnimUtil::noteToX(ctx.voiceNotes[hits[k]], ctx);
            const float vel = ctx.lastVelocity / 127.0f;
            poke(x, 60.0f + 140.0f * vel);
            const int drops = 2 + rnd(vel * 4.0f);
            for (int d = 0; d < drops && dropCount_ < MAX_DROPS; ++d) {
                Drop& dr = drops_[dropCount_++];
                dr.x = x;
                dr.y = surfaceAt(x) - 1.0f;
                dr.vx = rng_.range(-25.0f, 25.0f);
                dr.vy = -rng_.range(30.0f, 60.0f + 50.0f * vel);
            }
            // A loud note right under the duck kicks it straight up (the mesh alone can't build
            // enough upward speed from one hit to launch it)
            if (!duck_.air && vel >= KICK_MIN_VEL && std::fabs(x - duck_.x) < KICK_RANGE) {
                launch(-(50.0f + 90.0f * vel), vel >= FLIP_VEL);
            }
            duck_.target = x;
            quiet_ = 0.0f;
        }

        // Held voices shimmer at their key position
        for (int v = 0; v < 8; ++v) {
            const float env = ctx.voiceEnvelopes[v];
            if (env < 0.02f) continue;
            const float x = AnimUtil::noteToX(ctx.voiceNotes[v], ctx);
            const float w = AnimUtil::TWO_PI_F * (2.0f + (ctx.voiceNotes[v] - 36) / 12.0f * 1.5f);
            poke(x, std::sin(t_ * w) * env * 260.0f * dt);
        }
        if (ctx.activeVoiceCount > 0) quiet_ = 0.0f; else quiet_ += dt;

        // Spring mesh, 6 substeps (stable for K * sub^2 < 1)
        const float sub = dt / SUBSTEPS;
        for (int s = 0; s < SUBSTEPS; ++s) {
            for (int i = 0; i < N; ++i) {
                const float l = i > 0 ? h_[i - 1] : 0.0f;
                const float r = i < N - 1 ? h_[i + 1] : 0.0f;
                v_[i] += (K * (l + r - 2.0f * h_[i]) - KR * h_[i] - DAMP * v_[i]) * sub;
            }
            for (int i = 0; i < N; ++i) h_[i] = AnimUtil::clampf(h_[i] + v_[i] * sub, -14.0f, 14.0f);
        }

        // Droplets fall back and make tiny secondary splashes
        for (int i = 0; i < dropCount_; ++i) {
            Drop& d = drops_[i];
            d.vy += 240.0f * dt; d.x += d.vx * dt; d.y += d.vy * dt;
        }
        int w = 0;
        for (int i = 0; i < dropCount_; ++i) {
            const Drop d = drops_[i];
            if (d.vy > 0.0f && d.y >= surfaceAt(d.x)) { poke(d.x, 12.0f); continue; }
            if (d.x > -2.0f && d.x < 130.0f) drops_[w++] = d;
        }
        dropCount_ = w;

        // Duck
        Duck& duck = duck_;
        if (!duck.air) {
            const float dx = duck.target - duck.x;
            if (std::fabs(dx) > 2.0f) { duck.dir = dx > 0.0f ? 1 : -1; duck.x += duck.dir * 14.0f * dt; }
            const int i = rnd(AnimUtil::clampf(duck.x / 2.0f, 0.0f, N - 1.0f));
            if (v_[i] < LAUNCH_V) { // surface moving up fast under the duck: launch
                launch(v_[i] * 0.9f, v_[i] < FLIP_V);
            } else {
                duck.y = surfaceAt(duck.x) - (DUCK_H - 1);
            }
        } else {
            duck.airT += dt;
            duck.vy += 260.0f * dt;
            duck.y += duck.vy * dt;
            if (duck.vy > 0.0f && duck.y >= surfaceAt(duck.x) - (DUCK_H - 1)) {
                duck.air = false;
                poke(duck.x, duck.vy * 0.5f);
            }
        }
        duck.bang = std::fmax(0.0f, duck.bang - dt);
        quack_ = std::fmax(0.0f, quack_ - dt);

        if (quiet_ > SLEEP_AFTER) {
            zTimer_ -= dt;
            if (zTimer_ <= 0.0f && zCount_ < MAX_ZS) {
                zTimer_ = 1.4f;
                zs_[zCount_++] = {duck.x + 4.0f, duck.y - 4.0f, 0.0f};
            }
        }
        w = 0;
        for (int i = 0; i < zCount_; ++i) {
            Z z = zs_[i];
            z.y -= 7.0f * dt;
            z.t += dt;
            if (z.y > 12.0f && quiet_ > SLEEP_AFTER) zs_[w++] = z;
        }
        zCount_ = w;
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        // Surface polyline through the mesh nodes
        int px = 0, py = rnd(surfaceAt(0.0f));
        for (int i = 1; i < N; ++i) {
            const int x = i * 2, y = rnd(REST + h_[i]);
            AnimDraw::line(u8g2, px, py, x, y);
            px = x; py = y;
        }
        // Sparse drifting dots read as water on a 1-bit screen
        const int drift = static_cast<int>(std::floor(t_ * 4.0f));
        for (int x = 0; x < SCREEN_WIDTH; x += 4) {
            const int sy = static_cast<int>(std::ceil(surfaceAt(static_cast<float>(x)))) + 3;
            for (int y = sy; y <= 61; y += 3) {
                if (((x >> 2) + y / 3 + drift) % 4 == 0) AnimDraw::pixel(u8g2, x + ((y / 3) & 1) * 2, y);
            }
        }
        for (int i = 0; i < dropCount_; ++i) AnimDraw::pixel(u8g2, rnd(drops_[i].x), rnd(drops_[i].y));

        // 13x10, facing right; duck_.x is the centre column, duck_.y the top row
        static const char* const DUCK[DUCK_H] = {
            "........###..", ".......#####.", ".......##.##.", ".......######", "#......####..",
            "##....#####..", "###########..", "###########..", ".#########...", "..#######...."};
        static const char* const DUCK_QUACK[DUCK_H] = {
            "........###..", ".......#####.", ".......##.###", ".......###...", "#......######",
            "##....#####..", "###########..", "###########..", ".#########...", "..#######...."};
        static const char* const DUCK_SLEEP[DUCK_H] = {
            "........###..", ".......#####.", ".......#####.", ".......######", "#......####..",
            "##....#####..", "###########..", "###########..", ".#########...", "..#######...."};
        const char* const* rows = quiet_ > SLEEP_AFTER ? DUCK_SLEEP : (quack_ > 0.0f ? DUCK_QUACK : DUCK);
        bool upside = false;
        if (duck_.air && duck_.flip) { // tumble: upside down through the middle of the jump
            float phase = duck_.airT * 2.8f;
            phase -= std::floor(phase);
            upside = phase > 0.3f && phase < 0.75f;
        }
        const int dx = rnd(duck_.x), dy = rnd(duck_.y);
        AnimDraw::sprite(u8g2, dx - DUCK_HALF_W, dy, rows, DUCK_H, duck_.dir < 0, upside);
        if (duck_.bang > 0.0f) AnimDraw::tinyText(u8g2, dx - 1, dy - 7, "!");
        for (int i = 0; i < zCount_; ++i) {
            AnimDraw::tinyText(u8g2, rnd(zs_[i].x + zs_[i].t * 3.0f), rnd(zs_[i].y), "z");
        }
    }

    const char* getName() const override { return "Pond"; }

    void onBeat(uint8_t beatIndex, bool isDownbeat) override {
        (void)beatIndex;
        if (isDownbeat) quack_ = 0.18f;
    }

    // Water line (screen y) at x, interpolated between mesh nodes
    float surfaceAt(float x) const {
        const float f = AnimUtil::clampf(x / 2.0f, 0.0f, N - 1.001f);
        const int i = static_cast<int>(f);
        const float a = f - i;
        return REST + h_[i] * (1.0f - a) + h_[i + 1] * a;
    }

    // State, exposed for tests
    float getNodeHeight(int i) const { return h_[i]; }
    float getDuckX() const { return duck_.x; }
    float getDuckY() const { return duck_.y; }
    float getDuckTarget() const { return duck_.target; }
    bool isDuckAirborne() const { return duck_.air; }
    bool isDuckFlipping() const { return duck_.air && duck_.flip; }
    bool isQuacking() const { return quack_ > 0.0f; }
    bool isAsleep() const { return quiet_ > SLEEP_AFTER; }
    int getDropCount() const { return dropCount_; }
    int getZCount() const { return zCount_; }

    static constexpr int MAX_DROPS = 30;

private:
    static constexpr int MAX_ZS = 8;           // JS is unbounded; ~3 live at once in practice
    static constexpr int SUBSTEPS = 6;
    static constexpr float K = 2500.0f;        // neighbour spring
    static constexpr float KR = 6.0f;          // pull back to rest
    static constexpr float DAMP = 1.6f;
    static constexpr float LAUNCH_V = -55.0f;  // node velocity (px/s, up is negative) that launches the duck
    static constexpr float FLIP_V = -95.0f;    // ...and that makes it backflip
    static constexpr float KICK_RANGE = 12.0f;   // px: how close a note must land to the duck to kick it
    static constexpr float KICK_MIN_VEL = 0.55f; // normalised velocity needed to kick it
    static constexpr float FLIP_VEL = 0.85f;     // ...and to make that kick a backflip

    void launch(float vy, bool flip) {
        duck_.air = true;
        duck_.vy = vy;
        duck_.flip = flip;
        duck_.airT = 0.0f;
        duck_.bang = 0.5f;
    }
    static constexpr float SLEEP_AFTER = 6.0f;
    static constexpr int DUCK_H = 10;      // sprite rows
    static constexpr int DUCK_HALF_W = 6;  // sprite is 13 wide, centred on duck_.x

    struct Drop { float x, y, vx, vy; };
    struct Z { float x, y, t; };
    struct Duck {
        float x = 64.0f, y = REST - (DUCK_H - 1), vy = 0.0f;
        bool air = false, flip = false;
        float airT = 0.0f, target = 64.0f;
        int dir = 1;
        float bang = 0.0f;
    };

    // JS Math.round (halves round toward +infinity)
    static int rnd(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

    void poke(float x, float impulse) {
        const int i = rnd(AnimUtil::clampf(x / 2.0f, 1.0f, N - 2.0f));
        v_[i] += impulse;
        v_[i - 1] += impulse * 0.6f;
        v_[i + 1] += impulse * 0.6f;
    }

    AnimUtil::StrikeTracker tracker_;
    AnimUtil::XorShift32 rng_;
    float h_[N] = {};
    float v_[N] = {};
    Drop drops_[MAX_DROPS] = {};
    Z zs_[MAX_ZS] = {};
    int dropCount_ = 0;
    int zCount_ = 0;
    float t_ = 0.0f;
    float quiet_ = 0.0f;
    float quack_ = 0.0f;
    float zTimer_ = 0.0f;
    Duck duck_;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
