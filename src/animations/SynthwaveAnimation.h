#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// 80s synthwave sunset: a striped sun sinking into a neon perspective grid.
// - The grid scrolls toward you at the tempo: two grid lines per beat, locked to the metronome
//   when it is on, free-running at the set BPM when it is off.
// - Every note pulses the sun (harder velocity = bigger pulse).
// - The wireframe mountains on either side rise with the number and loudness of sounding voices.
class SynthwaveAnimation : public IAnimation {
public:
    void init() override { reset(); }

    void reset() override {
        gridPhase_ = 0.0f;
        sunPulse_ = 0.0f;
        mountainScale_ = MOUNTAIN_REST;
        stripeScroll_ = 0.0f;
        twinkle_ = 0;
    }

    void update(float dtSeconds, const AnimationContext& ctx) override {
        // Grid scroll: two lines per beat
        if (ctx.metronomeOn) {
            const float beats = ctx.beatPhase * LINES_PER_BEAT;
            gridPhase_ = beats - std::floor(beats);
        } else {
            gridPhase_ += (ctx.bpm / 60.0f) * LINES_PER_BEAT * dtSeconds;
            gridPhase_ -= std::floor(gridPhase_);
        }

        // Sun pulse decays; stripes drift upward like a CRT scanline sun
        sunPulse_ *= std::exp(-PULSE_DECAY * dtSeconds);
        stripeScroll_ += STRIPE_SPEED * dtSeconds;
        if (stripeScroll_ >= STRIPE_PERIOD) stripeScroll_ -= STRIPE_PERIOD;

        // Mountains follow the summed voice envelopes (a chord raises them)
        const float energy = AnimUtil::voiceEnergy(ctx);
        float target = MOUNTAIN_REST + (1.0f - MOUNTAIN_REST) * (energy / 3.0f > 1.0f ? 1.0f : energy / 3.0f);
        const float rate = target > mountainScale_ ? 6.0f : 1.5f; // rise fast, settle slowly
        mountainScale_ += (target - mountainScale_) * (rate * dtSeconds > 1.0f ? 1.0f : rate * dtSeconds);
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        renderStars(u8g2);
        renderSun(u8g2);
        renderMountains(u8g2);
        u8g2.drawHLine(0, HORIZON_Y, SCREEN_WIDTH);
        renderGrid(u8g2);
    }

    const char* getName() const override { return "Synthwave"; }

    void onNoteOn(uint8_t note, uint8_t velocity) override {
        (void)note;
        const float kick = 0.35f + 0.65f * (velocity / 127.0f);
        if (kick > sunPulse_) sunPulse_ = kick;
        ++twinkle_;
    }

    // State, exposed for tests
    float getSunRadius() const { return SUN_RADIUS + sunPulse_ * SUN_PULSE_PX; }
    float getMountainScale() const { return mountainScale_; }
    float getGridPhase() const { return gridPhase_; }

    static constexpr int HORIZON_Y = ANIM_REGION_TOP + 24; // y 40
    static constexpr int SUN_X = SCREEN_WIDTH / 2;

private:
    static constexpr float LINES_PER_BEAT = 2.0f;
    static constexpr float SUN_RADIUS = 14.0f;
    static constexpr float SUN_PULSE_PX = 4.0f;
    static constexpr float PULSE_DECAY = 5.0f;
    static constexpr float STRIPE_SPEED = 3.0f;     // rows / s
    static constexpr float STRIPE_PERIOD = 4.0f;
    static constexpr float MOUNTAIN_REST = 0.3f;
    static constexpr int GRID_ROWS = ANIM_REGION_BOTTOM - HORIZON_Y; // 22

    void renderStars(U8G2& u8g2) const {
        // Fixed star field; a different few blink off after each note
        static constexpr uint8_t STARS[][2] = {
            {6, 19}, {17, 27}, {28, 21}, {39, 30}, {44, 18}, {85, 20},
            {93, 29}, {104, 18}, {112, 25}, {122, 20}, {11, 33}, {118, 32},
        };
        for (int i = 0; i < 12; ++i) {
            if (((i * 7 + twinkle_) % 5) == 0) continue;
            AnimDraw::pixel(u8g2, STARS[i][0], STARS[i][1]);
        }
    }

    void renderSun(U8G2& u8g2) const {
        const float r = getSunRadius();
        const int rows = static_cast<int>(r);
        const int scroll = static_cast<int>(stripeScroll_);
        for (int dy = 1; dy <= rows; ++dy) {
            // Lower half of the sun is cut by scanline gaps that drift upward and thin out
            if (dy < rows / 2) {
                const int band = (dy + scroll) % static_cast<int>(STRIPE_PERIOD);
                const int gap = dy < rows / 4 ? 2 : 1;
                if (band < gap) continue;
            }
            const int half = static_cast<int>(std::sqrt(r * r - static_cast<float>(dy * dy)));
            AnimDraw::hspan(u8g2, SUN_X - half, SUN_X + half, HORIZON_Y - dy);
        }
    }

    void renderMountains(U8G2& u8g2) const {
        // Wireframe ranges either side of the sun: ridge polyline plus struts down to the horizon
        static constexpr int8_t LEFT[][2] = {{0, 6}, {8, 11}, {16, 7}, {24, 15}, {33, 8}, {42, 3}, {46, 0}};
        static constexpr int8_t RIGHT[][2] = {{82, 0}, {87, 5}, {95, 13}, {103, 8}, {111, 16}, {119, 9}, {127, 12}};
        drawRange(u8g2, LEFT, 7);
        drawRange(u8g2, RIGHT, 7);
    }

    void drawRange(U8G2& u8g2, const int8_t (*pts)[2], int n) const {
        int px = pts[0][0];
        int py = HORIZON_Y - static_cast<int>(pts[0][1] * mountainScale_);
        for (int i = 1; i < n; ++i) {
            const int x = pts[i][0];
            const int y = HORIZON_Y - static_cast<int>(pts[i][1] * mountainScale_);
            AnimDraw::line(u8g2, px, py, x, y);
            // Strut from each peak down to the valley floor between it and its neighbour
            if (pts[i][1] > pts[i - 1][1]) AnimDraw::line(u8g2, x, y, (px + x) / 2, HORIZON_Y);
            px = x;
            py = y;
        }
    }

    void renderGrid(U8G2& u8g2) const {
        // Horizontal lines at depths z = k + (1 - phase); screen row = horizon + GRID_ROWS / z.
        // Stop once lines would sit less than 3 px apart, so the far field doesn't merge into a band.
        int prevY = ANIM_REGION_BOTTOM + GRID_ROWS;
        for (int k = 0; k < 16; ++k) {
            const float z = static_cast<float>(k) + (1.0f - gridPhase_);
            if (z < 1.0f) continue;
            const int y = HORIZON_Y + static_cast<int>(GRID_ROWS / z + 0.5f);
            if (y < HORIZON_Y + 3 || prevY - y < 3) break;
            u8g2.drawHLine(0, y, SCREEN_WIDTH);
            prevY = y;
        }
        // Lines radiating from the vanishing point, starting just below the horizon
        // True perspective: x = centre + i * SPACING * depth, all converging on the sun. Outer lines
        // start further down so their near-horizontal far ends don't smear into a dashed band.
        constexpr int SPACING = 26;
        for (int i = -6; i <= 6; ++i) {
            const int absI = i < 0 ? -i : i;
            const int startRows = 3 + absI * 2;
            const int bottomX = SUN_X + i * SPACING;
            const int topX = SUN_X + (i * SPACING * startRows) / GRID_ROWS;
            AnimDraw::line(u8g2, topX, HORIZON_Y + startRows, bottomX, ANIM_REGION_BOTTOM);
        }
    }

    float gridPhase_ = 0.0f;       // 0..1 between grid lines
    float sunPulse_ = 0.0f;        // 0..1, decays
    float mountainScale_ = MOUNTAIN_REST;
    float stripeScroll_ = 0.0f;
    uint32_t twinkle_ = 0;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
