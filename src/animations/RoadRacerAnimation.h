#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>
#include <cstdio>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// 80s arcade-style pseudo-3D road racer, played from the keybed.
// - Steering: a note lower than the previous one steers left, a higher one steers right; the
//   bigger the interval, the harder the turn. Notes arriving together (a chord) don't steer.
// - Speed: more sounding voices = faster. With nothing playing the car cruises slowly.
// - The road bends on its own and bends push the car outward. Off the road the car slows
//   down and shakes, so you keep it on the tarmac by playing.
class RoadRacerAnimation : public IAnimation {
public:
    void init() override { reset(); }

    void reset() override {
        speed_ = IDLE_SPEED;
        playerX_ = 0.0f;
        steer_ = 0.0f;
        curvature_ = 0.0f;
        targetCurvature_ = 0.0f;
        distance_ = 0.0f;
        segmentLeft_ = FIRST_STRAIGHT;
        skyOffset_ = 0.0f;
        lastNote_ = -1;
        lastSteerMillis_ = 0;
        pendingNote_ = -1;
        frame_ = 0;
        rng_ = AnimUtil::XorShift32{};
    }

    void update(float dtSeconds, const AnimationContext& ctx) override {
        ++frame_;
        if (pendingNote_ >= 0) {
            applyNote(pendingNote_, ctx.lastNoteOnMillis);
            pendingNote_ = -1;
        }

        // Speed: idle cruise, then faster with every extra sounding voice. Off-road caps it.
        float target = IDLE_SPEED;
        if (ctx.activeVoiceCount >= 3)      target = 1.0f;
        else if (ctx.activeVoiceCount == 2) target = 0.75f;
        else if (ctx.activeVoiceCount == 1) target = 0.5f;
        if (isOffRoad() && target > OFFROAD_SPEED) target = OFFROAD_SPEED;
        const float rate = (target > speed_) ? ACCEL : BRAKE;
        speed_ += AnimUtil::clampf(target - speed_, -rate * dtSeconds, rate * dtSeconds);

        // Road layout: hold a curvature for a stretch, then pick a new one
        segmentLeft_ -= speed_ * dtSeconds * ROAD_UNITS_PER_SEC;
        if (segmentLeft_ <= 0.0f) nextSegment();
        curvature_ += AnimUtil::clampf(targetCurvature_ - curvature_, -CURVE_EASE * dtSeconds, CURVE_EASE * dtSeconds);

        // Lateral motion: steering minus the outward push of the bend, plus a weak pull to centre
        steer_ *= std::exp(-STEER_DECAY * dtSeconds);
        playerX_ += (steer_ * STEER_RATE - curvature_ * speed_ * DRIFT_RATE) * speed_ * dtSeconds;
        playerX_ -= playerX_ * CENTRE_PULL * dtSeconds;
        playerX_ = AnimUtil::clampf(playerX_, -MAX_X, MAX_X);

        distance_ += speed_ * dtSeconds * STRIPES_PER_SEC;
        if (distance_ > 1000.0f) distance_ -= 1000.0f;
        skyOffset_ += curvature_ * speed_ * dtSeconds * SKY_SCROLL;
        if (skyOffset_ < 0.0f) skyOffset_ += 256.0f;
        if (skyOffset_ >= 256.0f) skyOffset_ -= 256.0f;
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        renderSky(u8g2);
        renderRoad(u8g2);
        renderCar(u8g2);

        u8g2.setFont(u8g2_font_5x7_tf);
        char kmh[8];
        std::snprintf(kmh, sizeof(kmh), "%d", static_cast<int>(speed_ * 280.0f));
        u8g2.drawStr(1, ANIM_REGION_TOP + 7, kmh);
    }

    const char* getName() const override { return "Road Racer"; }

    void onNoteOn(uint8_t note, uint8_t velocity) override {
        (void)velocity;
        pendingNote_ = note;
    }

    // State, exposed for tests
    float getSpeed() const { return speed_; }
    float getPlayerX() const { return playerX_; }
    float getSteer() const { return steer_; }
    bool isOffRoad() const { return playerX_ > 1.0f || playerX_ < -1.0f; }

    // Layout (public for tests)
    static constexpr int HORIZON_Y = ANIM_REGION_TOP + 11;   // y 27
    static constexpr int ROAD_ROWS = ANIM_REGION_BOTTOM - HORIZON_Y;
    static constexpr int CAR_X = SCREEN_WIDTH / 2;
    static constexpr int CAR_BOTTOM = ANIM_REGION_BOTTOM;

private:
    static constexpr float IDLE_SPEED = 0.2f;
    static constexpr float OFFROAD_SPEED = 0.3f;
    static constexpr float ACCEL = 0.5f;              // speed units / s
    static constexpr float BRAKE = 0.35f;
    static constexpr float STEER_PER_SEMITONE = 1.0f / 7.0f;
    static constexpr float STEER_DECAY = 2.5f;        // 1/s
    static constexpr float STEER_RATE = 2.2f;
    static constexpr float DRIFT_RATE = 0.9f;
    static constexpr float CENTRE_PULL = 0.15f;
    static constexpr float MAX_X = 1.6f;
    static constexpr float CURVE_EASE = 0.8f;         // curvature units / s
    static constexpr float ROAD_UNITS_PER_SEC = 100.0f;
    static constexpr float FIRST_STRAIGHT = 150.0f;
    static constexpr float STRIPES_PER_SEC = 14.0f;
    static constexpr float SKY_SCROLL = 30.0f;
    static constexpr uint32_t CHORD_WINDOW_MS = 60;

    void applyNote(int note, uint32_t noteMillis) {
        const bool chord = lastNote_ >= 0 && noteMillis != 0 && noteMillis - lastSteerMillis_ < CHORD_WINDOW_MS;
        if (lastNote_ >= 0 && !chord) {
            int interval = note - lastNote_;
            if (interval > 12) interval = 12;
            if (interval < -12) interval = -12;
            steer_ = AnimUtil::clampf(steer_ + interval * STEER_PER_SEMITONE, -1.0f, 1.0f);
        }
        lastNote_ = note;
        lastSteerMillis_ = noteMillis;
    }

    void nextSegment() {
        static constexpr float CURVES[] = {-1.0f, -0.6f, -0.3f, 0.0f, 0.0f, 0.3f, 0.6f, 1.0f};
        const uint32_t r = rng_.next();
        targetCurvature_ = CURVES[r % 8];
        segmentLeft_ = 120.0f + static_cast<float>((r >> 8) % 240);
    }

    static int mountainHeight(int x) {
        // Two overlapping triangle waves: a low jagged ridge 1..4 px tall
        const int a = x % 46;
        const int b = (x + 11) % 18;
        const int ridge = (a < 23 ? a : 46 - a) / 8;
        const int peaks = (b < 9 ? b : 18 - b) / 5;
        return 1 + ridge + peaks;
    }

    void renderSky(U8G2& u8g2) const {
        // Striped synthwave sun sitting on the horizon, drifting against the bend
        const int sunX = CAR_X - static_cast<int>(curvature_ * 18.0f);
        constexpr int SUN_R = 10;
        for (int dy = 1; dy <= SUN_R; ++dy) {
            if (dy == 2 || dy == 4 || dy == 7) continue; // retro stripes
            const int half = static_cast<int>(std::sqrt(static_cast<float>(SUN_R * SUN_R - dy * dy)));
            AnimDraw::hspan(u8g2, sunX - half, sunX + half, HORIZON_Y - dy);
        }

        // Mountains in front of the sun: black fill under a white ridge line
        const int scroll = static_cast<int>(skyOffset_);
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            const int top = HORIZON_Y - mountainHeight((x + scroll) & 255);
            u8g2.setDrawColor(0);
            for (int y = top + 1; y < HORIZON_Y; ++y) u8g2.drawPixel(x, y);
            u8g2.setDrawColor(1);
            u8g2.drawPixel(x, top);
        }
        u8g2.drawHLine(0, HORIZON_Y, SCREEN_WIDTH);
    }

    void renderRoad(U8G2& u8g2) const {
        int prevSegment = -1;
        for (int y = HORIZON_Y + 1; y <= ANIM_REGION_BOTTOM; ++y) {
            const float p = static_cast<float>(y - HORIZON_Y) / ROAD_ROWS; // 0 far .. 1 near
            const float halfW = 2.0f + p * 56.0f;
            const float far = 1.0f - p;
            const float centre = CAR_X + curvature_ * far * far * 30.0f - playerX_ * halfW;
            const int left = static_cast<int>(centre - halfW);
            const int right = static_cast<int>(centre + halfW);
            const int segment = static_cast<int>(3.0f / p + distance_);
            const bool light = (segment & 1) != 0;

            if (light) {
                // Rumble strips, centre dash and grass texture on alternate segments
                const int rumble = 1 + static_cast<int>(halfW / 8.0f);
                AnimDraw::hspan(u8g2, left - rumble, left, y);
                AnimDraw::hspan(u8g2, right, right + rumble, y);
                if (halfW > 6.0f) {
                    const int dash = 1 + static_cast<int>(halfW / 24.0f);
                    AnimDraw::hspan(u8g2, static_cast<int>(centre) - dash / 2, static_cast<int>(centre) - dash / 2 + dash - 1, y);
                }
            } else {
                if (left >= 0 && left < SCREEN_WIDTH) u8g2.drawPixel(left, y);
                if (right >= 0 && right < SCREEN_WIDTH) u8g2.drawPixel(right, y);
            }

            // Roadside posts every 4th segment, growing as they rush toward the car
            if (prevSegment >= 0 && segment != prevSegment && (segment & 3) == 0 && p > 0.12f) {
                const int h = 1 + static_cast<int>(p * 9.0f);
                const int w = 1 + static_cast<int>(p * 2.0f);
                const int gap = 2 + static_cast<int>(halfW / 5.0f);
                drawPost(u8g2, left - gap - w, y, w, h);
                drawPost(u8g2, right + gap, y, w, h);
            }
            prevSegment = segment;
        }
    }

    static void drawPost(U8G2& u8g2, int x, int baseY, int w, int h) {
        int top = baseY - h + 1;
        if (top <= HORIZON_Y) top = HORIZON_Y + 1;
        for (int y = top; y <= baseY; ++y) AnimDraw::hspan(u8g2, x, x + w - 1, y);
    }

    void renderCar(U8G2& u8g2) const {
        const int shake = (isOffRoad() && (frame_ & 1)) ? 1 : 0;
        const int lean = static_cast<int>(std::lround(steer_ * 2.0f));
        const int x = CAR_X;
        const int b = CAR_BOTTOM - shake;

        // Clear a black pad so the road markings don't run through the car
        u8g2.setDrawColor(0);
        u8g2.drawBox(x - 11, b - 8, 22, 9);
        u8g2.setDrawColor(1);

        u8g2.drawBox(x - 5 + lean, b - 7, 10, 2);  // roof, leaning into the turn
        u8g2.drawBox(x - 9, b - 5, 18, 3);         // body
        u8g2.drawBox(x - 10, b - 2, 4, 3);         // wheels
        u8g2.drawBox(x + 6, b - 2, 4, 3);
        u8g2.setDrawColor(0);
        u8g2.drawBox(x - 7, b - 4, 3, 1);          // tail lights
        u8g2.drawBox(x + 4, b - 4, 3, 1);
        u8g2.setDrawColor(1);
    }

    float speed_ = IDLE_SPEED;   // 0..1
    float playerX_ = 0.0f;       // -1..1 = on the road, beyond = grass
    float steer_ = 0.0f;         // -1 (left) .. 1 (right), decays
    float curvature_ = 0.0f;     // -1 (left bend) .. 1 (right bend)
    float targetCurvature_ = 0.0f;
    float distance_ = 0.0f;
    float segmentLeft_ = FIRST_STRAIGHT;
    float skyOffset_ = 0.0f;
    int lastNote_ = -1;
    uint32_t lastSteerMillis_ = 0;
    int pendingNote_ = -1;
    uint32_t frame_ = 0;
    AnimUtil::XorShift32 rng_;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32
