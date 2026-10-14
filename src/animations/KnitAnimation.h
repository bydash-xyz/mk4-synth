#pragma once

#include "IAnimation.h"
#include "AnimDraw.h"
#include "AnimUtil.h"
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// A knitting machine that knits what you play into a scrolling sweater pattern (prototype:
// KnitAnimation at https://bydash.xyz/mk4-synth/animations/).
// - A carriage sweeps the needle bed back and forth, two rows per beat: locked to the metronome
//   when it is on, free-running at the set BPM when it is off.
// - Each of the 32 columns is a stretch of the keybed. Held notes knit V stitches in their column
//   (wider when the envelope is louder); every new note leaves a solid knot where it was struck.
// - Finished rows scroll down, so a held chord knits stripes, arpeggios diagonals, trills zig-zags.
// - After 4 s with no voice sounding the carriage parks ("PARK"); the next note restarts it.
// Notes come from the per-voice envelopes (StrikeTracker), so chords are seen whole.
class KnitAnimation : public IAnimation {
public:
    static constexpr int COLS = 32;
    static constexpr int MAXROWS = 12;
    static constexpr int TOPY = 26;
    static constexpr int ROWH = 3;

    void init() override { reset(); }

    void reset() override {
        tracker_.reset();
        rowCount_ = 0;
        std::memset(rows_, 0, sizeof(rows_));
        std::memset(curRow_, 0, sizeof(curRow_));
        std::memset(pending_, 0, sizeof(pending_));
        std::memset(heldCols_, 0, sizeof(heldCols_));
        freeBeat_ = 0.0f;
        hasLastIdx_ = false;
        lastIdx_ = 0;
        done_ = 0;
        quiet_ = 99.0f;
        parked_ = true;
        carX_ = 0.0f;
        dir_ = 1;
        commits_ = 0;
    }

    void update(float dt, const AnimationContext& ctx) override {
        uint8_t st[8];
        const int n = tracker_.update(ctx, st);
        for (int i = 0; i < n; ++i) {
            pending_[colFor(ctx.voiceNotes[st[i]], ctx)] = 2;
            quiet_ = 0.0f;
        }
        if (ctx.activeVoiceCount > 0) quiet_ = 0.0f;
        else quiet_ += dt;

        // Columns under a sounding voice, widened by its envelope
        std::memset(heldCols_, 0, sizeof(heldCols_));
        for (int v = 0; v < 8; ++v) {
            const float env = ctx.voiceEnvelopes[v];
            if (env < 0.05f) continue;
            const int c = colFor(ctx.voiceNotes[v], ctx);
            const int w = static_cast<int>(std::floor(env * 1.6f));
            for (int k = c - w; k <= c + w; ++k) if (k >= 0 && k < COLS) heldCols_[k] = 1;
        }

        freeBeat_ += ctx.bpm / 60.0f * dt;
        if (freeBeat_ >= 1024.0f) freeBeat_ -= 1024.0f;   // keep float precision; row parity unchanged
        const float rowPos = (ctx.metronomeOn ? ctx.currentBeat + ctx.beatPhase : freeBeat_) * ROWS_PER_BEAT;
        const int idx = static_cast<int>(std::floor(rowPos));
        const float p = rowPos - static_cast<float>(idx);
        if (quiet_ > PARK_AFTER) {
            if (!parked_) {
                commit();
                parked_ = true;
            }
            lastIdx_ = idx;
            hasLastIdx_ = true;
            return;
        }
        if (parked_) {
            parked_ = false;
            lastIdx_ = idx;
            hasLastIdx_ = true;
            done_ = 0;
        }
        if (!hasLastIdx_ || idx != lastIdx_) {
            knitTo(COLS);
            commit();
            lastIdx_ = idx;
            hasLastIdx_ = true;
        }
        knitTo(static_cast<int>(std::floor(p * COLS)));
        carX_ = dir_ > 0 ? p * 127.0f : 127.0f - p * 127.0f;
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        // Needle bed and carriage
        AnimDraw::hspan(u8g2, 0, 127, 23);
        for (int c = 0; c < COLS; ++c) AnimDraw::pixel(u8g2, c * 4 + 1, 24);
        const int cx = static_cast<int>(std::floor(carX_ + 0.5f));
        AnimDraw::rect(u8g2, cx - 7, 17, 15, 5);
        AnimDraw::box(u8g2, cx - 1, 19, 3, 1);

        // Row being knitted, then the finished fabric below it
        for (int k = 0; k < done_; ++k) {
            const int c = dir_ > 0 ? k : COLS - 1 - k;
            stitch(u8g2, c * 4, TOPY, curRow_[c], c % 2 == 0);
        }
        for (int i = 0; i < rowCount_; ++i) {
            const int y = TOPY + (i + 1) * ROWH;
            if (y + 1 > ANIM_REGION_BOTTOM) break;
            for (int c = 0; c < COLS; ++c) stitch(u8g2, c * 4, y, rows_[i][c], (c + i) % 2 == 1);
        }
        if (parked_) AnimDraw::tinyText(u8g2, 2, 17, "PARK");
    }

    const char* getName() const override { return "Knit"; }

    // State, exposed for tests
    static int colFor(int note, const AnimationContext& ctx) {
        const int c = static_cast<int>(std::floor(AnimUtil::noteToX(note, ctx, 2.0f) / 4.0f + 0.5f));
        return c < 0 ? 0 : (c > COLS - 1 ? COLS - 1 : c);
    }
    bool isParked() const { return parked_; }
    int getRowCount() const { return rowCount_; }
    uint8_t getRow(int i, int c) const { return rows_[i][c]; }       // 0 newest finished row
    uint32_t getCommits() const { return commits_; }                // rows finished since reset
    float getCarriageX() const { return carX_; }

private:
    static constexpr float ROWS_PER_BEAT = 2.0f;
    static constexpr float PARK_AFTER = 4.0f;

    // Finishes the current row: it becomes rows_[0] (empty rows only once the fabric has started)
    void commit() {
        bool any = false;
        for (int c = 0; c < COLS; ++c) any = any || curRow_[c] != 0;
        if (any || rowCount_ > 0) {
            const int keep = rowCount_ < MAXROWS ? rowCount_ : MAXROWS - 1;
            std::memmove(rows_[1], rows_[0], static_cast<size_t>(keep) * COLS);
            std::memcpy(rows_[0], curRow_, COLS);
            rowCount_ = keep + 1;
            ++commits_;
        }
        std::memset(curRow_, 0, sizeof(curRow_));
        dir_ = -dir_;
        done_ = 0;
    }

    void knitTo(int n) {
        for (; done_ < n; ++done_) {
            const int c = dir_ > 0 ? done_ : COLS - 1 - done_;
            curRow_[c] = pending_[c] ? 2 : heldCols_[c];
            pending_[c] = 0;
        }
    }

    static void stitch(U8G2& u8g2, int x, int y, uint8_t v, bool odd) {
        if (v == 2) {
            AnimDraw::box(u8g2, x, y, 3, 2);
        } else if (v == 1) {
            AnimDraw::pixel(u8g2, x, y);
            AnimDraw::pixel(u8g2, x + 2, y);
            AnimDraw::pixel(u8g2, x + 1, y + 1);
        } else if (odd) {
            AnimDraw::pixel(u8g2, x + 1, y + 1);
        }
    }

    AnimUtil::StrikeTracker tracker_;
    uint8_t rows_[MAXROWS][COLS] = {};   // finished rows, newest first: 0 empty, 1 stitch, 2 knot
    int rowCount_ = 0;
    uint8_t curRow_[COLS] = {};
    uint8_t pending_[COLS] = {};          // struck columns waiting for the carriage
    uint8_t heldCols_[COLS] = {};
    float freeBeat_ = 0.0f;
    bool hasLastIdx_ = false;
    int lastIdx_ = 0;
    int done_ = 0;                        // stitches knitted in the current row
    float quiet_ = 99.0f;
    bool parked_ = true;
    float carX_ = 0.0f;
    int dir_ = 1;
    uint32_t commits_ = 0;
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
