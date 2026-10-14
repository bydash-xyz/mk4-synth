#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include "KeyboardLayout.h"
#include <cmath>
#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// The top of a typewriter typing your melody's contour (prototype: TypewriterAnimation in
// docs/oled_midi_toys.html). No text at all, just the machine:
// - A tall sheet of paper (y16..44, three lines visible) over a platen roller with knobs at both
//   ends and a carriage-return lever off the left knob; it all slides left as you type.
// - Under the platen, a row of 13 little typebar hammers standing on pivots along the bottom
//   edge. Each struck note flips its hammer (low keys left, high keys right) up on its pivot until
//   the head hits the strike point, stamping one mark set higher on the line for higher keys, so
//   each typed line traces the melody. Velocity picks the ink: hard = solid block, medium = hollow
//   box, soft = dot. Two notes struck together stack in one column.
// - Three or more notes at once jam: the struck hammers' heads tangle at the paper and nothing is
//   typed until the jam clears.
// - A short pause types a space, the bell in the corner shakes near the margin, and a full line or
//   a 3 s pause returns the carriage (the paper feeds up; two finished lines stay visible).
// Notes come from the per-voice envelopes (StrikeTracker), so chords are seen whole.
class TypewriterAnimation : public IAnimation {
public:
    static constexpr int COLS = 22;
    static constexpr int BARS = 13;          // hammers
    static constexpr int KEYS = KeyboardLayout::KEY_COUNT;
    static constexpr int MAX_RISES = 2;      // 3+ simultaneous notes jam, so a cell holds at most 2
    static constexpr int PREV_LINES = 2;     // finished lines kept on the paper

    // Geometry
    static constexpr int SX = 64;            // strike point
    static constexpr int STRIKE_Y = 44;
    static constexpr int LINE_BOTTOM = 43;   // bottom row of the current line's marks
    static constexpr int LINE_PITCH = 12;    // line feed
    static constexpr int PLATEN_TOP = 45;
    static constexpr int PLATEN_BOTTOM = 48;
    static constexpr int HAMMER_X0 = 16;     // first pivot; pivots are 8 px apart on the bottom row
    static constexpr int HAMMER_STEP = 8;
    static constexpr int PIVOT_Y = 62;
    static constexpr int BELL_X = 116;
    static constexpr int BELL_Y = 52;

    // One typed column: count 0 is a space
    struct Cell {
        uint8_t count;
        uint8_t rises[MAX_RISES];  // 0..8 px above the line's baseline mark
        uint8_t ink;               // 0 dot, 1 hollow, 2 solid
    };

    void init() override { reset(); }

    void reset() override {
        tracker_.reset();
        curCount_ = 0;
        for (int& c : prevCount_) c = 0;
        prevLines_ = 0;
        col_ = 0;
        shownCol_ = 0.0f;
        feed_ = 0.0f;
        barCount_ = 0;
        jam_ = 0.0f;
        jamCount_ = 0;
        ding_ = 0.0f;
        silence_ = 0.0f;
        spaced_ = true;
        blink_ = 0.0f;
        returning_ = false;
    }

    void update(float dt, const AnimationContext& ctx) override {
        blink_ += dt;
        blink_ -= std::floor(blink_);   // only the 1 s cursor blink cycle matters

        uint8_t st[8];
        const int n = tracker_.update(ctx, st);
        jam_ = jam_ - dt > 0.0f ? jam_ - dt : 0.0f;
        if (n >= 3 && jam_ <= 0.0f) {
            jam_ = JAM_TIME;
            jamCount_ = n;
            for (int i = 0; i < n; ++i) jamBars_[i] = static_cast<uint8_t>(barFor(keyOf(ctx.voiceNotes[st[i]], ctx)));
        } else if (n > 0 && jam_ <= 0.0f) {
            Cell cell{};
            cell.count = static_cast<uint8_t>(n);
            for (int i = 0; i < n; ++i) {
                const int k = keyOf(ctx.voiceNotes[st[i]], ctx);
                if (barCount_ < MAX_SWINGS) {
                    swings_[barCount_].bar = static_cast<uint8_t>(barFor(k));
                    swings_[barCount_].t = 0.0f;
                    ++barCount_;
                }
                cell.rises[i] = static_cast<uint8_t>(rnd(static_cast<float>(k) / (KEYS - 1) * MAX_RISE_PX));
            }
            cell.ink = ctx.lastVelocity >= 100 ? 2 : (ctx.lastVelocity >= 45 ? 1 : 0);
            advance(cell);
            spaced_ = false;
        }
        if (n > 0) {
            silence_ = 0.0f;
        } else {
            silence_ += dt;
            if (silence_ > SPACE_PAUSE && !spaced_ && col_ > 0) {
                spaced_ = true;
                advance(Cell{});
            }
            if (silence_ > RETURN_PAUSE && col_ > 0) carriageReturn();
        }

        // Hammer swings age out after one strike
        int keep = 0;
        for (int i = 0; i < barCount_; ++i) {
            swings_[i].t += dt;
            if (swings_[i].t < SWING_TIME) swings_[keep++] = swings_[i];
        }
        barCount_ = keep;

        const float rate = returning_ ? 7.0f : 30.0f;
        const float k = dt * rate < 1.0f ? dt * rate : 1.0f;
        shownCol_ += (static_cast<float>(col_) - shownCol_) * k;
        if (returning_ && std::fabs(static_cast<float>(col_) - shownCol_) < 0.1f) returning_ = false;
        feed_ *= std::exp(-dt * 12.0f);
        ding_ = ding_ - dt > 0.0f ? ding_ - dt : 0.0f;
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        // Paper and typed marks, sliding with the carriage
        const int paperX = rnd(SX - shownCol_ * 4.0f);
        const int bottom = LINE_BOTTOM + rnd(feed_);
        const int left = paperX - 6, right = paperX + COLS * 4 + 4;
        AnimDraw::vspan(u8g2, left, 16, STRIKE_Y);
        AnimDraw::vspan(u8g2, right, 16, STRIKE_Y);
        drawCells(u8g2, cur_, curCount_, paperX, bottom);
        for (int l = 0; l < prevLines_; ++l) {
            drawCells(u8g2, prev_[l], prevCount_[l], paperX, bottom - LINE_PITCH * (l + 1));
        }
        // Cursor blinks at the strike point when idle
        if (silence_ > SPACE_PAUSE && blink_ < 0.5f) AnimDraw::hspan(u8g2, SX, SX + 2, STRIKE_Y);

        // Platen roller with knobs, and the carriage-return lever on the left
        AnimDraw::hspan(u8g2, left - 4, right + 4, PLATEN_TOP);
        AnimDraw::hspan(u8g2, left - 4, right + 4, PLATEN_BOTTOM);
        AnimDraw::vspan(u8g2, left - 4, PLATEN_TOP, PLATEN_BOTTOM);
        AnimDraw::vspan(u8g2, right + 4, PLATEN_TOP, PLATEN_BOTTOM);
        AnimDraw::box(u8g2, left - 8, PLATEN_TOP, 3, 4);
        AnimDraw::box(u8g2, right + 6, PLATEN_TOP, 3, 4);
        AnimDraw::line(u8g2, left - 8, PLATEN_TOP - 1, left - 13, PLATEN_TOP - 6);
        AnimDraw::box(u8g2, left - 15, PLATEN_TOP - 7, 3, 2);

        // Hammers: an arm from the pivot to a 3x2 head. A struck one flips up on its pivot until
        // its head hits the strike point; jammed heads pile up there.
        for (int i = 0; i < BARS; ++i) {
            const float px = pivotX(i), py = PIVOT_Y;
            const float hx = restHeadX(i), hy = static_cast<float>(restHeadY(i));
            float f = 0.0f;
            for (int b = 0; b < barCount_; ++b) {
                if (swings_[b].bar == i) {
                    f = std::sin(AnimUtil::PI_F * swings_[b].t / SWING_TIME);
                    break;
                }
            }
            float tx = SX, ty = STRIKE_Y + 1;
            if (jam_ > 0.0f && isJamBar(i)) {
                f = 0.9f;
                tx += (static_cast<float>(i) - BARS / 2.0f) * 0.8f;
                ty += static_cast<float>(i % 3);
            }
            // Swing: rotate the arm about its pivot from rest toward the target, stretching to reach
            const float a0 = std::atan2(hy - py, hx - px), l0 = std::hypot(hx - px, hy - py);
            float a1 = std::atan2(ty - py, tx - px);
            while (a1 - a0 > AnimUtil::PI_F) a1 -= AnimUtil::TWO_PI_F;
            while (a0 - a1 > AnimUtil::PI_F) a1 += AnimUtil::TWO_PI_F;
            const float a = a0 + (a1 - a0) * f;
            const float len = l0 + (std::hypot(tx - px, ty - py) - l0) * f;
            const float ex = px + std::cos(a) * len, ey = py + std::sin(a) * len;
            AnimDraw::line(u8g2, rnd(px), rnd(py), rnd(ex), rnd(ey));
            AnimDraw::box(u8g2, rnd(ex) - 1, rnd(ey) - 1, 3, 2);
        }

        // Bell in the corner, shaking when it dings
        static const char* const BELL[4] = {"..#..", ".###.", ".###.", "#####"};
        const int shake = ding_ > 0.0f ? ((static_cast<int>(std::floor(ding_ * 30.0f)) % 2) ? 1 : -1) : 0;
        AnimDraw::sprite(u8g2, BELL_X + shake, BELL_Y, BELL, 4);
        if (ding_ > 0.0f) {
            AnimDraw::pixel(u8g2, BELL_X - 2, BELL_Y - 1);
            AnimDraw::pixel(u8g2, BELL_X + 6, BELL_Y - 1);
            AnimDraw::pixel(u8g2, BELL_X - 3, BELL_Y + 1);
            AnimDraw::pixel(u8g2, BELL_X + 7, BELL_Y + 1);
        }
    }

    const char* getName() const override { return "Typewriter"; }

    // State, exposed for tests
    int getColumn() const { return col_; }
    const Cell& getCell(int i) const { return cur_[i]; }
    int getPreviousLines() const { return prevLines_; }
    bool hasPreviousLine() const { return prevLines_ > 0; }
    int getPreviousLineLength(int l = 0) const { return prevCount_[l]; }
    const Cell& getPreviousCell(int i, int l = 0) const { return prev_[l][i]; }
    bool isJammed() const { return jam_ > 0.0f; }
    bool isDinging() const { return ding_ > 0.0f; }
    bool isBarSwinging(int i) const {
        for (int b = 0; b < barCount_; ++b) if (swings_[b].bar == i) return true;
        return false;
    }

    // Hammer i stands on a pivot along the bottom edge, leaning a little toward the middle, with
    // its head on a gentle arc (higher in the middle)
    static int pivotX(int i) { return HAMMER_X0 + i * HAMMER_STEP; }
    static float restHeadX(int i) { return pivotX(i) + (SX - pivotX(i)) * 0.2f; }
    static int restHeadY(int i) {
        const float off = std::fabs(static_cast<float>(pivotX(i) - SX)) / 48.0f;
        return PIVOT_Y - 8 - rnd((1.0f - off) * 3.0f);
    }
    static int barFor(int key) { return rnd(static_cast<float>(key) / (KEYS - 1) * (BARS - 1)); }

private:
    static constexpr float JAM_TIME = 0.8f;
    static constexpr float SWING_TIME = 0.16f;
    static constexpr float SPACE_PAUSE = 0.6f;
    static constexpr float RETURN_PAUSE = 3.0f;
    static constexpr float MAX_RISE_PX = 8.0f;
    static constexpr int MAX_SWINGS = 16;

    struct Swing { uint8_t bar; float t; };

    static int rnd(float v) { return static_cast<int>(std::floor(v + 0.5f)); }  // JS Math.round

    static int keyOf(int note, const AnimationContext& ctx) {
        const int k = note - ctx.keybedNoteOffset - KeyboardLayout::BASE_NOTE;
        return k < 0 ? 0 : (k > KEYS - 1 ? KEYS - 1 : k);
    }

    bool isJamBar(int i) const {
        for (int j = 0; j < jamCount_; ++j) if (jamBars_[j] == i) return true;
        return false;
    }

    void carriageReturn() {
        // Older lines move up one slot; the line just finished becomes prev_[0]
        for (int l = PREV_LINES - 1; l > 0; --l) {
            for (int i = 0; i < prevCount_[l - 1]; ++i) prev_[l][i] = prev_[l - 1][i];
            prevCount_[l] = prevCount_[l - 1];
        }
        for (int i = 0; i < curCount_; ++i) prev_[0][i] = cur_[i];
        prevCount_[0] = curCount_;
        if (prevLines_ < PREV_LINES) ++prevLines_;
        curCount_ = 0;
        col_ = 0;
        returning_ = true;
        feed_ = static_cast<float>(LINE_PITCH);
        spaced_ = true;
    }

    void advance(const Cell& cell) {
        cur_[curCount_++] = cell;
        ++col_;
        if (col_ == COLS - 4) ding_ = 0.6f;
        if (col_ >= COLS) carriageReturn();
    }

    // A line is 11 px tall: the mark's top sits 0..8 px above the line's bottom by pitch
    static void drawCells(U8G2& u8g2, const Cell* cells, int count, int x0, int bottom) {
        for (int i = 0; i < count; ++i) {
            const Cell& c = cells[i];
            const int x = x0 + i * 4;
            for (int r = 0; r < c.count; ++r) {
                const int y = bottom - 2 - c.rises[r];
                if (c.ink == 2) AnimDraw::box(u8g2, x, y, 3, 3);
                else if (c.ink == 1) AnimDraw::rect(u8g2, x, y, 3, 3);
                else AnimDraw::pixel(u8g2, x + 1, y + 1);
            }
        }
    }

    AnimUtil::StrikeTracker tracker_;
    Cell cur_[COLS] = {};
    Cell prev_[PREV_LINES][COLS] = {};
    int curCount_ = 0;
    int prevCount_[PREV_LINES] = {};
    int prevLines_ = 0;
    int col_ = 0;
    float shownCol_ = 0.0f;     // eased column the paper is drawn at
    float feed_ = 0.0f;         // line-feed offset in px, decays to 0
    Swing swings_[MAX_SWINGS] = {};
    int barCount_ = 0;
    float jam_ = 0.0f;          // seconds left of the jam
    uint8_t jamBars_[8] = {};
    int jamCount_ = 0;
    float ding_ = 0.0f;
    float silence_ = 0.0f;
    bool spaced_ = true;
    float blink_ = 0.0f;        // 0..1 cursor blink phase
    bool returning_ = false;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
