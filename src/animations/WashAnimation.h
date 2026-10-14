#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>
#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// A laundromat: three front-loading washing machines side by side.
// - The keybed is split in thirds: low keys load the left machine, middle keys the centre one and
//   high keys the right one.
// - Each note (every voice that strikes, so chords work) throws a sock, a t-shirt or a pair of
//   shorts (picked by note % 3) into its machine, where it tumbles under gravity and is dragged
//   along by the drum wall. At most 5 garments per machine; the oldest is taken out first, and
//   anything older than 12 s is removed.
// - The drums turn a quarter turn per beat. Each machine's water level follows the summed voice
//   envelopes in its third, and hard hits (velocity) rock that machine only.
// - Holding 2+ voices in one third for over a second ramps up that machine's spin cycle: its drum
//   turns 6x faster, clothes stick to the wall and the cabinet shakes.
// - Two panel lights per machine; the lit one chases across all six lights on the beat
//   (metronome beat, or free-running at the BPM).
class WashAnimation : public IAnimation {
public:
    static constexpr int M = 3;           // machines
    static constexpr int W = 42;          // px per machine (cabinet is W - 1 wide)
    static constexpr int CY = 44;         // drum centre y
    static constexpr int R = 9;           // drum radius
    static constexpr int MAX_CLOTHES = 5; // per machine

    // Drum centre x of machine i
    static constexpr int drumX(int i) { return 1 + i * W + 20; }

    // Machine a note loads: low third left, high third right
    static int machineFor(int note, const AnimationContext& ctx) {
        const int i = static_cast<int>(std::floor(AnimUtil::noteToX(note, ctx, 0.0f) / 128.0f * M));
        return i < M - 1 ? (i < 0 ? 0 : i) : M - 1;
    }

    WashAnimation() { reset(); }

    void init() override { reset(); }

    void reset() override {
        tracker_.reset();
        rng_.state = 0x9E3779B9u;
        t_ = 0.0f;
        freeBeat_ = 0.0f;
        beat_ = 0;
        for (int i = 0; i < M; ++i) {
            Machine& m = machines_[i];
            m.clothCount = 0;
            m.drum = i * DRUM_PHASE_STEP;
            m.drumSpeed = 0.0f;
            m.spin = 0.0f;
            m.spinHold = 0.0f;
            m.water = 0.0f;
            m.shake = 0.0f;
        }
    }

    void update(float dt, const AnimationContext& ctx) override {
        t_ += dt;
        uint8_t hits[8];
        const int n = tracker_.update(ctx, hits);
        for (int h = 0; h < n; ++h) {
            const int note = ctx.voiceNotes[hits[h]];
            const int i = machineFor(note, ctx);
            Machine& m = machines_[i];
            if (m.clothCount >= MAX_CLOTHES) dropOldest(m);
            Cloth& c = m.clothes[m.clothCount++];
            c.x = drumX(i) + rng_.range(-3.0f, 3.0f);
            c.y = CY - 5.0f;
            c.vx = rng_.range(-15.0f, 15.0f);
            c.vy = 20.0f;
            c.kind = static_cast<uint8_t>(note % 3);
            c.flip = rng_.uniform() < 0.5f;
            c.age = 0.0f;
            const float kick = ctx.lastVelocity / 127.0f;
            if (kick > m.shake) m.shake = kick;
        }

        float energy[M] = {};
        int held[M] = {};
        for (int v = 0; v < 8; ++v) {
            const float env = ctx.voiceEnvelopes[v];
            if (env < 0.02f) continue;
            const int i = machineFor(ctx.voiceNotes[v], ctx);
            energy[i] += env;
            if (env > 0.05f) ++held[i];
        }

        freeBeat_ += ctx.bpm / 60.0f * dt;
        if (freeBeat_ > 3000.0f) freeBeat_ -= 3000.0f; // multiple of 6 so the panel lights don't jump
        beat_ = ctx.metronomeOn ? ctx.currentBeat : static_cast<int>(std::floor(freeBeat_));

        const float ease2 = AnimUtil::clampf(dt * 2.0f, 0.0f, 1.0f);
        const float ease3 = AnimUtil::clampf(dt * 3.0f, 0.0f, 1.0f);
        for (int i = 0; i < M; ++i) {
            Machine& m = machines_[i];
            const float cx = static_cast<float>(drumX(i));
            m.spinHold = held[i] >= 2 ? m.spinHold + dt : 0.0f;
            m.spin += ((m.spinHold > SPIN_HOLD_S ? 1.0f : 0.0f) - m.spin) * ease2;
            m.drumSpeed = ctx.bpm / 60.0f * (AnimUtil::PI_F / 2.0f) * (1.0f + m.spin * 5.0f);
            m.drum += m.drumSpeed * dt;
            if (m.drum > AnimUtil::TWO_PI_F * 64.0f) m.drum -= AnimUtil::TWO_PI_F * 64.0f; // keep float precision; whole turns only
            const float targetWater = energy[i] / 1.5f < 1.0f ? energy[i] / 1.5f : 1.0f;
            m.water += (targetWater - m.water) * ease3;

            const float maxD = R - 2.5f;
            const float level = CY + R - m.water * R * 1.2f;
            for (int k = 0; k < m.clothCount; ++k) {
                Cloth& c = m.clothes[k];
                c.age += dt;
                c.vy += GRAVITY * (1.0f - m.spin * 0.7f) * dt;
                float dx = c.x - cx, dy = c.y - CY;
                float d = std::sqrt(dx * dx + dy * dy);
                if (d == 0.0f) d = 0.001f;
                // Spin pins clothes to the wall
                c.vx += dx / d * m.spin * SPIN_PUSH * dt;
                c.vy += dy / d * m.spin * SPIN_PUSH * dt;
                if (c.y > level) { c.vx *= 0.94f; c.vy *= 0.94f; }
                c.x += c.vx * dt;
                c.y += c.vy * dt;
                dx = c.x - cx;
                dy = c.y - CY;
                d = std::sqrt(dx * dx + dy * dy);
                if (d == 0.0f) d = 0.001f;
                if (d > maxD) {
                    const float nx = dx / d, ny = dy / d;
                    c.x = cx + nx * maxD;
                    c.y = CY + ny * maxD;
                    const float vr = c.vx * nx + c.vy * ny;
                    if (vr > 0.0f) { c.vx -= vr * nx * 1.3f; c.vy -= vr * ny * 1.3f; }
                    // The drum wall drags the clothes along with it
                    const float tx = -ny, ty = nx, vt = c.vx * tx + c.vy * ty;
                    const float dv = (m.drumSpeed * R - vt) * 0.5f;
                    c.vx += dv * tx;
                    c.vy += dv * ty;
                }
            }
            // Take out clothes older than 12 s (order kept)
            int kept = 0;
            for (int k = 0; k < m.clothCount; ++k) {
                if (m.clothes[k].age < MAX_AGE_S) m.clothes[kept++] = m.clothes[k];
            }
            m.clothCount = kept;
            m.shake = m.shake - dt * 3.0f > 0.0f ? m.shake - dt * 3.0f : 0.0f;
        }
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        static constexpr const char* SOCK[4] = {"##.", "##.", "###", ".##"};
        static constexpr const char* SHIRT[3] = {"#####", ".###.", ".###."};
        static constexpr const char* SHORTS[3] = {"###", "#.#", "#.#"};
        static constexpr const char* const* SPRITES[3] = {SOCK, SHIRT, SHORTS};
        static constexpr int SPRITE_W[3] = {3, 5, 3};
        static constexpr int SPRITE_H[3] = {4, 3, 3};
        const int lit = getLitLight();

        for (int i = 0; i < M; ++i) {
            const Machine& m = machines_[i];
            const int ox = rnd(std::sin(t_ * 60.0f + i) * m.shake * 1.5f +
                               std::sin(t_ * 47.0f + i * 2.0f) * m.spin * 1.2f);
            const int x0 = 1 + i * W + ox;
            const int cx = drumX(i) + ox;

            // Cabinet, control panel with the chasing lights, dial and detergent drawer
            AnimDraw::rect(u8g2, x0, 17, W - 1, 46);
            AnimDraw::hspan(u8g2, x0, x0 + W - 2, 23);
            for (int j = 0; j < 2; ++j) {
                if (lit == i * 2 + j) AnimDraw::box(u8g2, x0 + 4 + j * 5, 19, 3, 3);
                else AnimDraw::rect(u8g2, x0 + 4 + j * 5, 19, 3, 3);
            }
            AnimDraw::circle(u8g2, x0 + 33, 20, 2);
            AnimDraw::pixel(u8g2, x0 + 33 + rnd(std::cos(m.drum) * 1.4f), 20 + rnd(std::sin(m.drum) * 1.4f));
            AnimDraw::rect(u8g2, x0 + 4, 25, 10, 3);

            // Door and drum with three ribs
            AnimDraw::circle(u8g2, cx, CY, R + 3);
            AnimDraw::circle(u8g2, cx, CY, R + 1, 2);
            for (int k = 0; k < 3; ++k) {
                const float a = m.drum + k * AnimUtil::TWO_PI_F / 3.0f;
                const float ca = std::cos(a), sa = std::sin(a);
                AnimDraw::line(u8g2, rnd(cx + ca * (R - 1)), rnd(CY + sa * (R - 1)),
                               rnd(cx + ca * (R - 3)), rnd(CY + sa * (R - 3)));
            }

            // Water: a wavy surface with a sparse dither below it, inside the drum
            if (m.water > 0.05f) {
                const float level = CY + R - m.water * R * 1.2f;
                for (int x = -R; x <= R; ++x) {
                    const int y = rnd(level + std::sin(x * 0.6f + t_ * 8.0f + i) * (1.0f + m.spin));
                    if (x * x + (y - CY) * (y - CY) < R * R) AnimDraw::pixel(u8g2, cx + x, y);
                    for (int yy = y + 3; yy < CY + R; yy += 3) {
                        if ((x + yy) % 4 == 0 && x * x + (yy - CY) * (yy - CY) < R * R) AnimDraw::pixel(u8g2, cx + x, yy);
                    }
                }
            }

            for (int k = 0; k < m.clothCount; ++k) {
                const Cloth& c = m.clothes[k];
                AnimDraw::sprite(u8g2, rnd(c.x) + ox - (SPRITE_W[c.kind] >> 1), rnd(c.y) - (SPRITE_H[c.kind] >> 1),
                                 SPRITES[c.kind], SPRITE_H[c.kind], c.flip);
            }
        }
    }

    const char* getName() const override { return "Laundromat"; }

    // State, exposed for tests
    int getClothCount(int m) const { return machines_[m].clothCount; }
    float getClothX(int m, int i) const { return machines_[m].clothes[i].x; }
    float getClothY(int m, int i) const { return machines_[m].clothes[i].y; }
    int getClothKind(int m, int i) const { return machines_[m].clothes[i].kind; }
    float getSpin(int m) const { return machines_[m].spin; }
    float getDrumSpeed(int m) const { return machines_[m].drumSpeed; }
    float getWater(int m) const { return machines_[m].water; }
    float getShake(int m) const { return machines_[m].shake; }
    int getBeat() const { return beat_; }
    // Index 0..5 of the lit panel light (machine * 2 + light)
    int getLitLight() const { return ((beat_ % (2 * M)) + 2 * M) % (2 * M); }

private:
    static constexpr float GRAVITY = 300.0f;        // px/s^2, eased off by the spin
    static constexpr float SPIN_PUSH = 400.0f;      // outward px/s^2 at full spin
    static constexpr float SPIN_HOLD_S = 1.0f;      // hold 2+ voices in a third this long to spin
    static constexpr float MAX_AGE_S = 12.0f;
    static constexpr float DRUM_PHASE_STEP = 2.1f;  // starting drum angle offset between machines

    struct Cloth {
        float x, y, vx, vy, age;
        uint8_t kind;
        bool flip;
    };

    struct Machine {
        Cloth clothes[MAX_CLOTHES];
        int clothCount;
        float drum, drumSpeed, spin, spinHold, water, shake;
    };

    static int rnd(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

    static void dropOldest(Machine& m) {
        for (int i = 0; i < m.clothCount - 1; ++i) m.clothes[i] = m.clothes[i + 1];
        --m.clothCount;
    }

    AnimUtil::StrikeTracker tracker_;
    AnimUtil::XorShift32 rng_;
    Machine machines_[M];
    float t_ = 0.0f, freeBeat_ = 0.0f;
    int beat_ = 0;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
