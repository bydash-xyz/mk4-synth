#pragma once

#include "IAnimation.h"
#include "KeyboardLayout.h"

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {

// Default animation: the MPK mini MK4's 25-key keybed drawn as a piano.
// Each key is bound 1-to-1 to the physical key on the controller (the MPK's own octave and
// transpose are undone via ctx.keybedNoteOffset). A sounding key fills from the bottom in
// proportion to the amp envelope of the voice playing it, and stays partly filled through the
// release until the voice has fully faded out. Notes that fall off the keybed are not drawn.
class KeyboardAnimation : public IAnimation {
public:
    void init() override {}
    void reset() override {
        for (float& l : displayLevels_) l = 0.0f;
    }

    void update(float dtSeconds, const AnimationContext& ctx) override {
        (void)dtSeconds;
        float target[KeyboardLayout::KEY_COUNT] = {0};
        for (int v = 0; v < 8; ++v) {
            const float env = ctx.voiceEnvelopes[v];
            if (env <= 0.0f) continue;
            const int key = KeyboardLayout::keyIndexForNote(ctx.voiceNotes[v], ctx.keybedNoteOffset);
            if (key >= 0 && env > target[key]) target[key] = env;  // retriggered notes: loudest wins
        }
        for (int k = 0; k < KeyboardLayout::KEY_COUNT; ++k) {
            if (target[k] > displayLevels_[k]) {
                displayLevels_[k] = target[k]; // Instant attack
            } else {
                displayLevels_[k] += (target[k] - displayLevels_[k]) * 0.15f; // Smooth decay
            }
            if (displayLevels_[k] < 0.005f) displayLevels_[k] = 0.0f;
        }
    }

    void render(U8G2& u8g2, const AnimationContext& ctx) override {
        (void)ctx;
        using namespace KeyboardLayout;

        // White keys first (they share borders), then black keys on top.
        for (int k = 0; k < KEY_COUNT; ++k) {
            if (isBlackKey(k)) continue;
            const int x = whiteKeyX(whiteColumn(k));
            u8g2.drawFrame(x, KEY_TOP, WHITE_W, WHITE_H);
            drawFill(u8g2, x + 1, KEY_TOP + 1, WHITE_W - 2, WHITE_H - 2, displayLevels_[k]);
        }
        for (int k = 0; k < KEY_COUNT; ++k) {
            if (!isBlackKey(k)) continue;
            const int x = blackKeyX(k);
            u8g2.setDrawColor(0);                       // knock the black key out of the white keys
            u8g2.drawBox(x, KEY_TOP, BLACK_W, BLACK_H);
            u8g2.setDrawColor(1);
            u8g2.drawFrame(x, KEY_TOP, BLACK_W, BLACK_H);
            drawFill(u8g2, x + 1, KEY_TOP + 1, BLACK_W - 2, BLACK_H - 2, displayLevels_[k]);
        }
    }

    const char* getName() const override { return "Keyboard"; }

private:
    // Fills the bottom `level` fraction of the inner rectangle.
    static void drawFill(U8G2& u8g2, int x, int y, int w, int h, float level) {
        if (level <= 0.01f) return;
        int fill = static_cast<int>(level * h + 0.5f);
        if (fill < 1) fill = 1;
        if (fill > h) fill = h;
        u8g2.drawBox(x, y + h - fill, w, fill);
    }

    float displayLevels_[KeyboardLayout::KEY_COUNT] = {0};
};

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32
