// Unit tests for the MIDI toy OLED animations (src/animations/*Animation.h, prototyped in
// docs/oled_midi_toys.html). Each animation's tests live in tests/toy_anims/<name>_test.inc.
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include <U8g2lib.h>
#include "animations/AnimDraw.h"
#include "animations/AnimUtil.h"
#include "animations/ChoirAnimation.h"
#include "animations/PondAnimation.h"
#include "animations/PlinkoAnimation.h"
#include "animations/TypewriterAnimation.h"
#include "animations/KnitAnimation.h"
#include "animations/WashAnimation.h"
#include "animations/SeismoAnimation.h"

using namespace HardwareDriver;

namespace {

// Stand-in for SynthEngine's voice snapshot: 8 voices with a fast ADSR, written into an
// AnimationContext the way OledDisplayUI::buildAnimationContext does.
struct ToyVoices {
    struct Voice { uint8_t note = 0; float env = 0.0f; int stage = 0; uint32_t age = 0; }; // 0 off, 1 a, 2 d, 3 s, 4 r
    Voice v[8];
    uint32_t ages = 0;
    AnimationContext ctx{};

    ToyVoices() { ctx.bpm = 120.0f; ctx.timeSigNumerator = 4; ctx.timeSigDenominator = 4; ctx.lastVelocity = 100; }

    void noteOn(uint8_t note, uint8_t vel) {
        Voice* slot = nullptr;
        for (auto& x : v) if (x.stage != 0 && x.note == note) slot = &x;
        if (!slot) for (auto& x : v) if (x.stage == 0) { slot = &x; break; }
        if (!slot) { slot = &v[0]; for (auto& x : v) if (x.age < slot->age) slot = &x; }
        slot->note = note; slot->stage = 1; slot->age = ++ages;
        ctx.lastNote = note; ctx.lastVelocity = vel; ctx.lastNoteOnMillis += 1;
    }
    void noteOff(uint8_t note) {
        for (auto& x : v) if (x.note == note && x.stage >= 1 && x.stage <= 3) x.stage = 4;
    }
    // Advances the envelopes by dt and refreshes ctx
    void step(float dt) {
        for (auto& x : v) {
            if (x.stage == 1) { x.env += dt / 0.012f; if (x.env >= 1.0f) { x.env = 1.0f; x.stage = 2; } }
            else if (x.stage == 2) { x.env -= 0.4f * dt / 0.25f; if (x.env <= 0.6f) { x.env = 0.6f; x.stage = 3; } }
            else if (x.stage == 4) { x.env *= std::exp(-dt / 0.12f); if (x.env < 0.002f) { x.env = 0.0f; x.stage = 0; } }
        }
        ctx.activeVoiceCount = 0;
        for (int i = 0; i < 8; ++i) {
            ctx.voiceNotes[i] = v[i].stage ? v[i].note : 0;
            ctx.voiceEnvelopes[i] = v[i].env;
            if (v[i].stage) ctx.activeVoiceCount++;
        }
    }
};

// Steps voices and animation together for `seconds` at the firmware's 20 FPS
template <typename A>
void run(A& anim, ToyVoices& tv, float seconds, float dt = 0.05f) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += dt) {
        tv.step(dt);
        anim.update(dt, tv.ctx);
    }
}

// Shows an animation one silent frame, as the device does before anything can be played into it.
// StrikeTracker ignores the first frame after reset() (it only records ringing voices then).
template <typename A>
void prime(A& anim) {
    AnimationContext silent{};
    silent.bpm = 120.0f;
    anim.update(0.0f, silent);
}

// Nothing may be drawn in the yellow header (y 0..15) or on the reserved bottom row (y 63)
bool blueZoneOnly(const U8G2& d) {
    for (int x = 0; x < 128; ++x) {
        for (int y = 0; y < ANIM_REGION_TOP; ++y) if (d.getPixel(x, y)) return false;
        if (d.getPixel(x, 63)) return false;
    }
    return true;
}

int litPixels(const U8G2& d, int x0 = 0, int y0 = 0, int x1 = 127, int y1 = 63) {
    int n = 0;
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) n += d.getPixel(x, y) ? 1 : 0;
    return n;
}

void test_toy_kit() {
    std::cout << "[Test] AnimUtil..." << std::endl;
    ToyVoices tv;
    AnimUtil::StrikeTracker tr;
    uint8_t out[8];
    tv.step(0.05f);
    assert(tr.update(tv.ctx, out) == 0);
    // A three-note chord inside one frame is three strikes (onNoteOn would only see one)
    tv.noteOn(48, 100); tv.noteOn(52, 100); tv.noteOn(55, 100);
    tv.step(0.05f);
    assert(tr.update(tv.ctx, out) == 3);
    tv.step(0.05f);
    assert(tr.update(tv.ctx, out) == 0); // held: no repeat
    tv.noteOff(48); tv.noteOff(52); tv.noteOff(55);
    for (int i = 0; i < 4; ++i) { tv.step(0.05f); tr.update(tv.ctx, out); }
    tv.noteOn(60, 100);
    tv.step(0.05f);
    assert(tr.update(tv.ctx, out) == 1);

    // After reset() (switching to an animation) voices still ringing are not new strikes
    tv.noteOff(60);
    tv.step(0.05f);
    tr.reset();
    tv.step(0.05f);
    assert(tr.update(tv.ctx, out) == 0);
    tv.step(0.05f);
    assert(tr.update(tv.ctx, out) == 0);
    tv.noteOn(64, 100);
    tv.step(0.05f);
    assert(tr.update(tv.ctx, out) == 1);

    // noteToX spans the keybed across the screen and follows the octave offset
    AnimationContext ctx{};
    assert(std::fabs(AnimUtil::noteToX(48, ctx, 0.0f) - 0.0f) < 0.01f);
    assert(std::fabs(AnimUtil::noteToX(72, ctx, 0.0f) - 127.0f) < 0.01f);
    ctx.keybedNoteOffset = 12;
    assert(std::fabs(AnimUtil::noteToX(60, ctx, 0.0f) - 0.0f) < 0.01f);

    // Drawing helpers clip to the blue zone
    U8G2 d;
    d.begin();
    AnimDraw::circle(d, 64, 16, 10);
    AnimDraw::disc(d, 0, 62, 5);
    AnimDraw::tinyText(d, 2, 12, "DING");
    assert(blueZoneOnly(d));
    std::cout << "  -> AnimUtil tests PASSED!" << std::endl;
}

} // namespace

#include "toy_anims/choir_test.inc"
#include "toy_anims/pond_test.inc"
#include "toy_anims/plinko_test.inc"
#include "toy_anims/typewriter_test.inc"
#include "toy_anims/knit_test.inc"
#include "toy_anims/wash_test.inc"
#include "toy_anims/seismo_test.inc"

int main() {
    test_toy_kit();
    test_choir();
    test_pond();
    test_plinko();
    test_typewriter();
    test_knit();
    test_wash();
    test_seismo();
    std::cout << "\n>>> ALL TOY ANIMATION TESTS PASSED <<<" << std::endl;
    return 0;
}
