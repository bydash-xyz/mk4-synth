#pragma once

#include <cstdint>
#include <cstddef>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
#include <U8g2lib.h>
#endif

namespace HardwareDriver {

// Snapshot of synth & MIDI telemetry passed to visualizers each frame.
// Intentionally small (~32 bytes) and copyable — no pointers to audio buffers.
// Gathered on Core 0 from SynthEngine state to avoid cross-core locking.
struct AnimationContext {
    // Clock & Rhythm
    float bpm;                     // Current BPM (40–300)
    uint8_t timeSigNumerator;      // e.g. 4 in 4/4
    uint8_t timeSigDenominator;    // e.g. 4 in 4/4
    uint8_t currentBeat;           // 0..(numerator-1)
    float beatPhase;               // 0.0..1.0 continuous within current beat
    bool isDownbeat;               // true when currentBeat == 0
    bool metronomeOn;              // false: beat fields are frozen (phase 0), animate from notes instead

    // Voice Activity
    uint8_t activeVoiceCount;      // 0..8
    uint8_t voiceNotes[8];         // MIDI note per voice slot (0 if inactive)
    float voiceEnvelopes[8];       // 0.0..1.0 amp envelope level per voice

    // Last MIDI Event
    uint8_t lastNote;              // 0..127
    uint8_t lastVelocity;          // 0..127
    uint32_t lastNoteOnMillis;     // millis() timestamp of last noteOn

    // Semitone shift the MPK applies to keybed notes (octave/transpose); subtract it from a
    // received note to get the physical key. 0 at centre octave with no transpose.
    int16_t keybedNoteOffset;

    // Master Output
    float masterVolume;            // 0.0..1.0

    // Frame Timing
    uint32_t nowMillis;            // current millis()
};

// Animation rendering region constants (matches SSD1306 dual-color hardware zones)
static constexpr int ANIM_REGION_TOP    = 16;  // First blue-zone pixel row
static constexpr int ANIM_REGION_BOTTOM = 62;  // Last usable row (y=63 reserved for floor)
static constexpr int ANIM_REGION_HEIGHT = ANIM_REGION_BOTTOM - ANIM_REGION_TOP; // 46 px
static constexpr int SCREEN_WIDTH       = 128;

// Abstract base class for all OLED animations.
// Implementations render into the blue zone (y 16–62) of the 128×64 SSD1306.
// The yellow zone (y 0–15) is reserved for the stats header and managed by OledDisplayUI.
class IAnimation {
public:
    virtual ~IAnimation() = default;

    // Called once at startup
    virtual void init() = 0;

    // Called when switching to this animation (clear internal state)
    virtual void reset() = 0;

    // Non-blocking physics/timer update. dtSeconds is clamped to [0, 0.05].
    virtual void update(float dtSeconds, const AnimationContext& ctx) = 0;

    // Render into U8g2 buffer. MUST only draw within y=[ANIM_REGION_TOP..ANIM_REGION_BOTTOM].
#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
    virtual void render(U8G2& u8g2, const AnimationContext& ctx) = 0;
#endif

    // Human-readable name for UI display (max 16 chars)
    virtual const char* getName() const = 0;

    // Optional low-latency event hooks
    virtual void onNoteOn(uint8_t note, uint8_t velocity) { (void)note; (void)velocity; }
    virtual void onNoteOff(uint8_t note) { (void)note; }
    virtual void onBeat(uint8_t beatIndex, bool isDownbeat) { (void)beatIndex; (void)isDownbeat; }
};

} // namespace HardwareDriver
