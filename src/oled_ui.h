#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>
#include "SynthEngine.h"
#include "Presets.h"
#include "knob_page.h"
#include "system_status.h"
#include "animations/IAnimation.h"
#include "animations/AnimationRegistry.h"
#include "version.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#elif defined(MOCK_HAL)
#include "Arduino.h"
#include "Wire.h"
#include "U8g2lib.h"
#endif

namespace HardwareDriver {

// =========================================================================
// 0.96" SSD1306 OLED Display UI Engine (I2C: GPIO 8 SDA, GPIO 9 SCL)
//
// Layout (128×64, ELEGOO dual-color split):
//   Yellow zone (y 0–15):  Compact stats header — preset, BPM, time sig, STEREO/MONO
//   Blue zone   (y 16–62): Animation (delegated to AnimationRegistry)
//   Floor line  (y 63):    Reserved
//
// Knob HUD overlay temporarily replaces the animation zone for 1.5s.
//
// System faults (SystemStatus, optional): a fatal fault replaces the whole screen with a fault page (drawn
// and sent once per change); warnings add an inverted tag under the header stats plus a 3 s banner at the
// bottom of the blue zone; until the MK4 first enumerates the blue zone shows "Connect MPK Mini MK4".
// With no faults the only extra per-frame work is reading the fault mask.
// =========================================================================
class OledDisplayUI {
public:
    static constexpr int OLED_SDA_PIN = 8;
    static constexpr int OLED_SCL_PIN = 9;
    static constexpr uint32_t HUD_DISPLAY_DURATION_MS = 1500;
    static constexpr uint32_t ANIM_NAME_BANNER_MS = 1000;

    OledDisplayUI()
        : activePresetIndex_(0),
          lastKnobTouchMillis_(0),
          hudKnobValue_(0),
          currentPage_(KnobPage::BANK_A),
          needsRedraw_(true),
          animRegistry_(nullptr),
          lastFrameMicros_(0) {
        std::memset(presetName_, 0, sizeof(presetName_));
        const auto& presets = Synth::getPresets();
        if (!presets.empty()) {
            std::strncpy(presetName_, presets[0].name, 16);
        }
        std::memset(hudKnobName_, 0, sizeof(hudKnobName_));
    }

    ~OledDisplayUI() = default;

    bool init() {
#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
        Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, 400000); // 400kHz Fast I2C
        u8g2_.begin();
        u8g2_.clearBuffer();
        u8g2_.setFont(u8g2_font_6x10_tf);
        u8g2_.drawStr(10, 25, "MK4-SYNTH");
        char verStr[32];
        std::snprintf(verStr, sizeof(verStr), "%s Ready", MK4_SYNTH_VERSION);
        if (std::strlen(verStr) * 6 + 10 <= 128) {
            u8g2_.drawStr(10, 45, verStr);
        } else {
            u8g2_.drawStr(10, 45, MK4_SYNTH_VERSION);
        }
        u8g2_.sendBuffer();
        lastFrameMicros_ = micros();
#endif
        needsRedraw_ = true;
        return true;
    }

    // Connect the system fault model (call once during setup)
    void setSystemStatus(SystemStatus* status) {
        status_ = status;
        lastFatalDrawn_ = 0;
    }

    // Connect the animation registry (call once during setup)
    void setAnimationRegistry(AnimationRegistry* registry) {
        animRegistry_ = registry;
    }

    // Semitone shift applied by the MPK to keybed notes (see AnimationContext::keybedNoteOffset)
    void setKeybedNoteOffset(int semitones) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        keybedNoteOffset_ = static_cast<int16_t>(semitones);
    }

    void setPreset(uint8_t index, const char* name) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activePresetIndex_ = index;
        std::memset(presetName_, 0, sizeof(presetName_));
        if (name) {
            std::strncpy(presetName_, name, 16);
        }
        needsRedraw_ = true;
    }

    void setPage(KnobPage page) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentPage_ = page;
        needsRedraw_ = true;
    }

    void notifyKnobTouched(const char* paramName, uint8_t value, const char* valueText = nullptr) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        std::memset(hudKnobName_, 0, sizeof(hudKnobName_));
        if (paramName) {
            std::strncpy(hudKnobName_, paramName, 15);
        }
        std::memset(hudKnobText_, 0, sizeof(hudKnobText_));
        if (valueText) {
            std::strncpy(hudKnobText_, valueText, sizeof(hudKnobText_) - 1);
        }
        hudKnobValue_ = value;
        lastKnobTouchMillis_ = millis();
        needsRedraw_ = true;
    }

    // Convenience: cycle animations via registry
    void nextAnimation() {
        if (animRegistry_) animRegistry_->next();
    }

    // Queue an animation step (+1 next / -1 previous) from another task (e.g. the USB MIDI task).
    // Applied at the start of the next render() so the animation is never reset mid-frame.
    void requestAnimationStep(int step) {
        pendingAnimStep_.fetch_add(static_cast<int8_t>(step > 0 ? 1 : -1));
    }

    // Call regularly from Core 0 loop (e.g. ~30 FPS = every 33ms)
    void render(const Synth::SynthEngine& engine, float masterVolume,
                const AnimationContext& animCtx) {
        uint32_t now = millis();
        applyPendingAnimationStep(now);

        UiStateSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            snap.activePresetIndex = activePresetIndex_;
            std::memcpy(snap.presetName, presetName_, sizeof(snap.presetName));
            snap.currentPage = currentPage_;
            std::memcpy(snap.hudKnobName, hudKnobName_, sizeof(snap.hudKnobName));
            snap.hudKnobValue = hudKnobValue_;
            std::memcpy(snap.hudKnobText, hudKnobText_, sizeof(snap.hudKnobText));
            snap.lastKnobTouchMillis = lastKnobTouchMillis_;
        }

        bool showHud = (now - snap.lastKnobTouchMillis < HUD_DISPLAY_DURATION_MS)
                       && (snap.hudKnobName[0] != '\0');

        uint8_t activeVoices = static_cast<uint8_t>(engine.getActiveVoiceCount());

        // Compute delta time for animation update
        float dtSeconds = 0.033f; // default ~30 FPS
#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
        uint32_t nowUs = micros();
        if (lastFrameMicros_ > 0) {
            dtSeconds = static_cast<float>(nowUs - lastFrameMicros_) * 1e-6f;
            if (dtSeconds > 0.05f) dtSeconds = 0.05f; // Clamp to prevent physics explosions
            if (dtSeconds < 0.001f) dtSeconds = 0.001f;
        }
        lastFrameMicros_ = nowUs;
#endif

        lastRenderedPreset_ = snap.activePresetIndex;
        lastRenderedVoices_ = activeVoices;
        lastRenderedVol_ = masterVolume;
        // Detect note-on and beat transitions in AnimationContext and notify active animation
        if (animRegistry_ && animRegistry_->getActive()) {
            IAnimation* anim = animRegistry_->getActive();
            if (animCtx.lastNoteOnMillis != 0 && animCtx.lastNoteOnMillis != lastSeenNoteOnMillis_) {
                lastSeenNoteOnMillis_ = animCtx.lastNoteOnMillis;
                anim->onNoteOn(animCtx.lastNote, animCtx.lastVelocity);
            }
            if (animCtx.currentBeat != lastSeenBeat_) {
                lastSeenBeat_ = animCtx.currentBeat;
                anim->onBeat(animCtx.currentBeat, animCtx.isDownbeat);
            }
        }

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
        const uint16_t faults = status_ ? status_->mask() : 0;
        if (faults & SystemStatus::FATAL_MASK) {
            // Static page: redraw and resend only when the set of fatal faults changes
            const uint16_t fatal = faults & SystemStatus::FATAL_MASK;
            if (fatal != lastFatalDrawn_) {
                lastFatalDrawn_ = fatal;
                u8g2_.clearBuffer();
                renderFaultPage(u8g2_, fatal);
                u8g2_.sendBuffer();
            }
            needsRedraw_ = false;
            return;
        }
        lastFatalDrawn_ = 0;
        const bool waitingForMpk = status_ && status_->isWaitingForMpk();

        u8g2_.clearBuffer();

        // =================================================================
        // YELLOW ZONE (y 0–15): Compact Stats Header
        // =================================================================
        renderStatsHeader(u8g2_, engine, snap);
        if (faults) {
            renderWarningTag(u8g2_, faults);
        }

        // Separator: dashed line at y=15 (bottom of yellow zone)
        for (int x = 0; x < 128; x += 4) {
            u8g2_.drawPixel(x, 15);
            u8g2_.drawPixel(x + 1, 15);
        }

        // =================================================================
        // BLUE ZONE (y 16–62): Animation or Knob HUD Overlay
        // =================================================================
        if (waitingForMpk) {
            renderWaitingForMpk(u8g2_, (faults & faultBit(SystemFault::MpkMissing)) != 0);
        } else if (showHud) {
            renderKnobHud(u8g2_, engine, snap);
        } else if (animRegistry_ && animRegistry_->getActive()) {
            // Update & render the active animation
            IAnimation* anim = animRegistry_->getActive();
            anim->update(dtSeconds, animCtx);
            anim->render(u8g2_, animCtx);
        }

        SystemFault bannerFault = SystemFault::Count;
        if (faults && !waitingForMpk && status_->bannerFault(now, bannerFault)) {
            renderWarningBanner(u8g2_, SystemStatus::info(bannerFault).banner);
        } else if (!waitingForMpk && !showHud && animBannerMillis_ != 0
                   && now - animBannerMillis_ < ANIM_NAME_BANNER_MS) {
            renderWarningBanner(u8g2_, animRegistry_->getActiveName());
        }

        u8g2_.sendBuffer();
#else
        if (animRegistry_ && animRegistry_->getActive()) {
            animRegistry_->getActive()->update(dtSeconds, animCtx);
        }
#endif
        needsRedraw_ = false;
    }

    // Legacy render signature (backward compatible, builds context internally)
    void render(const Synth::SynthEngine& engine, float masterVolume) {
        AnimationContext ctx = buildAnimationContext(engine, masterVolume);
        render(engine, masterVolume, ctx);
    }

    uint8_t getActivePreset() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return activePresetIndex_;
    }
    const char* getPresetName() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return presetName_;
    }
    uint8_t getLastRenderedVoices() const { return lastRenderedVoices_; }
    float getLastRenderedVol() const { return lastRenderedVol_; }
#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
    U8G2* getDisplay() { return &u8g2_; }
    const U8G2* getDisplay() const { return &u8g2_; }
#endif

private:
#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2_{U8G2_R0, U8X8_PIN_NONE};

    // =====================================================================
    // Stats Header: fits in yellow zone (y 0–14, 15px tall)
    // Layout: "P1 Juno-60" on left, "120 4/4 ST" on right (no voice count, so the metronome fits).
    // Output mode instead of volume: master volume is on K16, and a mono left-only output is easy to forget.
    // =====================================================================
    struct UiStateSnapshot {
        uint8_t  activePresetIndex{0};
        char     presetName[17]{};
        KnobPage currentPage{KnobPage::BANK_A};
        char     hudKnobName[16]{};
        uint8_t  hudKnobValue{0};
        char     hudKnobText[12]{};
        uint32_t lastKnobTouchMillis{0};
    };

    void renderStatsHeader(U8G2& u8g2, const Synth::SynthEngine& engine, const UiStateSnapshot& snap) {
        // Left side: Preset slot + name (bold font)
        u8g2.setFont(u8g2_font_6x10_tf);
        char presetBuf[20];
        std::snprintf(presetBuf, sizeof(presetBuf), "P%d %.9s",
                      snap.activePresetIndex + 1, snap.presetName);
        u8g2.drawStr(0, 10, presetBuf);
        // Right side: compact stats (small font)
        u8g2.setFont(u8g2_font_5x7_tf);

        // Build right-aligned stats string
        char statsBuf[24];
        const bool mono = engine.isMonoOutput();
        if (engine.isMetronomeEnabled()) {
            // Short form so "240 12/8" still clears the preset name
            std::snprintf(statsBuf, sizeof(statsBuf), "%d %s %s",
                          static_cast<int>(engine.getMetronomeBpm()),
                          engine.getTimeSignatureString(),
                          mono ? "MONO" : "ST");
        } else {
            std::snprintf(statsBuf, sizeof(statsBuf), "%s", mono ? "MONO" : "STEREO");
        }
        int statsWidth = u8g2.getStrWidth(statsBuf);
        u8g2.drawStr(128 - statsWidth, 7, statsBuf);

        // Beat flash under the tempo (also with a silent click): thick on beat 1, thin on the others
        if (engine.isMetronomeEnabled() && engine.getBeatPhase() < 0.3f) {
            u8g2.drawBox(128 - statsWidth, 10, statsWidth, engine.getCurrentBeat() == 0 ? 3 : 1);
        }
    }

    // Centred text on one row (drawn with the current font)
    static void drawCentred(U8G2& u8g2, int baselineY, const char* s) {
        int x = (128 - u8g2.getStrWidth(s)) / 2;
        u8g2.drawStr(x < 0 ? 0 : x, baselineY, s);
    }

    // =====================================================================
    // Fatal fault page (whole screen): inverted title banner in the yellow zone, centred hint lines in the
    // blue zone, serial pointer at the bottom
    // =====================================================================
    void renderFaultPage(U8G2& u8g2, uint16_t fatalMask) {
        const FaultInfo& fi = SystemStatus::info(SystemStatus::first(fatalMask));

        u8g2.drawBox(0, 0, 128, 15);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_6x10_tf);
        drawCentred(u8g2, 11, fi.title);
        u8g2.setDrawColor(1);

        drawCentred(u8g2, 32, fi.hint1);
        drawCentred(u8g2, 44, fi.hint2);

        u8g2.setFont(u8g2_font_5x7_tf);
        drawCentred(u8g2, ANIM_REGION_BOTTOM,
                    SystemStatus::countBits(fatalMask) > 1 ? "more faults: serial log" : "details: serial log");
    }

    // =====================================================================
    // Warning tag: small inverted label under the right-hand header stats (rows 8..14, free space below the
    // 5x7 stats line). Shows the highest-priority warning, "+" when there are more.
    // =====================================================================
    void renderWarningTag(U8G2& u8g2, uint16_t faults) {
        const uint16_t warnings = faults & static_cast<uint16_t>(~SystemStatus::FATAL_MASK);
        if (!warnings) return;
        char tag[12];
        std::snprintf(tag, sizeof(tag), "%s%s", SystemStatus::info(SystemStatus::first(warnings)).tag,
                      SystemStatus::countBits(warnings) > 1 ? "+" : "");
        u8g2.setFont(u8g2_font_5x7_tf);
        const int w = u8g2.getStrWidth(tag) + 3;
        u8g2.drawBox(128 - w, 8, w, 7);
        u8g2.setDrawColor(0);
        u8g2.drawStr(128 - w + 2, 14, tag);
        u8g2.setDrawColor(1);
    }

    // Transient warning banner across the bottom of the blue zone (rows 53..62)
    static void renderWarningBanner(U8G2& u8g2, const char* text) {
        u8g2.setDrawColor(0);
        u8g2.drawBox(0, 52, 128, 1); // gap line so the banner reads over busy animations
        u8g2.setDrawColor(1);
        u8g2.drawBox(0, 53, 128, 10);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_5x7_tf);
        drawCentred(u8g2, 61, text);
        u8g2.setDrawColor(1);
    }

    // Boot waiting page in the blue zone until the MK4 first enumerates on USB Host
    static void renderWaitingForMpk(U8G2& u8g2, bool timedOut) {
        u8g2.setFont(u8g2_font_6x10_tf);
        drawCentred(u8g2, 32, "Connect");
        drawCentred(u8g2, 44, "MPK Mini MK4");
        u8g2.setFont(u8g2_font_5x7_tf);
        drawCentred(u8g2, ANIM_REGION_BOTTOM, timedOut ? "not found - check cable" : "to the USB host port");
    }

    // =====================================================================
    // Knob HUD Overlay: temporarily replaces animation zone (y 16–48)
    // Shows parameter name, value bar, and numeric value
    // =====================================================================
    void renderKnobHud(U8G2& u8g2, const Synth::SynthEngine& engine, const UiStateSnapshot& snap) {
        u8g2.setFont(u8g2_font_6x10_tf);

        // Parameter name
        u8g2.drawStr(4, 30, snap.hudKnobName);

        // Value bar
        u8g2.drawFrame(4, 35, 120, 8);
        int barWidth = (snap.hudKnobValue * 116) / 127;
        if (barWidth > 0) {
            u8g2.drawBox(6, 37, barWidth, 4);
        }

        // Numeric value or tone name
        char valStr[16];
        if (snap.hudKnobText[0]) {
            u8g2.drawStr(124 - u8g2.getStrWidth(snap.hudKnobText), 30, snap.hudKnobText);
        } else if (std::strcmp(snap.hudKnobName, "Tone") == 0) {
            std::snprintf(valStr, sizeof(valStr), "%s", engine.getCasioToneName());
            u8g2.drawStr(70, 30, valStr);
        } else {
            std::snprintf(valStr, sizeof(valStr), "%d", snap.hudKnobValue);
            u8g2.drawStr(100, 30, valStr);
        }

        // Page indicator at bottom of HUD zone
        u8g2.setFont(u8g2_font_5x7_tf);
        const char* pageStr = "[BANK A]";
        if (snap.currentPage == KnobPage::BANK_B) {
            pageStr = "[BANK B]";
        } else if (snap.currentPage == KnobPage::RHYTHM) {
            pageStr = "[RHYTHM]";
        }
        u8g2.drawStr(2, ANIM_REGION_BOTTOM, pageStr);

        // Animation name on right (so user knows what they'll return to)
        if (animRegistry_ && animRegistry_->getActive()) {
            const char* animName = animRegistry_->getActiveName();
            int nameWidth = u8g2.getStrWidth(animName);
            u8g2.drawStr(128 - nameWidth - 2, ANIM_REGION_BOTTOM, animName);
        }
    }
#endif

    void applyPendingAnimationStep(uint32_t now) {
        int8_t step = pendingAnimStep_.exchange(0);
        if (step == 0 || !animRegistry_) return;
        for (; step > 0; --step) animRegistry_->next();
        for (; step < 0; ++step) animRegistry_->prev();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            hudKnobName_[0] = '\0';  // drop the knob HUD so the new animation shows right away
        }
        animBannerMillis_ = now ? now : 1;
        needsRedraw_ = true;
    }

    // Build AnimationContext from SynthEngine state
    AnimationContext buildAnimationContext(const Synth::SynthEngine& engine,
                                           float masterVolume) {
        AnimationContext ctx{};
        ctx.bpm = engine.getMetronomeBpm();
        ctx.timeSigNumerator = engine.getTimeSigBeatsPerBar();
        ctx.timeSigDenominator = engine.getTimeSigBeatUnit();
        ctx.currentBeat = engine.getCurrentBeat();
        ctx.beatPhase = engine.getBeatPhase();
        ctx.isDownbeat = (ctx.currentBeat == 0);
        ctx.metronomeOn = engine.isMetronomeEnabled();
        ctx.activeVoiceCount = static_cast<uint8_t>(engine.getActiveVoiceCount());
        ctx.lastNote = engine.getLastNoteNumber();
        ctx.lastVelocity = engine.getLastVelocity();
        ctx.lastNoteOnMillis = engine.getLastNoteOnMillis();
        ctx.masterVolume = masterVolume;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            ctx.keybedNoteOffset = keybedNoteOffset_;
        }
        ctx.nowMillis = millis();

        // Populate per-voice snapshot
        engine.getVoiceSnapshot(ctx.voiceNotes, ctx.voiceEnvelopes, ctx.activeVoiceCount);

        return ctx;
    }

    mutable std::mutex stateMutex_;
    int16_t keybedNoteOffset_{0};
    uint8_t activePresetIndex_;
    char presetName_[17];
    uint32_t lastKnobTouchMillis_;
    char hudKnobName_[16];
    uint8_t hudKnobValue_;
    char hudKnobText_[12]{};
    KnobPage currentPage_;
    bool needsRedraw_;
    AnimationRegistry* animRegistry_;
    uint32_t lastFrameMicros_;
    SystemStatus* status_{nullptr};
    uint16_t lastFatalDrawn_{0};

    uint8_t lastRenderedPreset_{0};
    uint8_t lastRenderedVoices_{0};
    float lastRenderedVol_{0.0f};
    uint32_t lastSeenNoteOnMillis_{0};
    uint8_t lastSeenBeat_{255};
    std::atomic<int8_t> pendingAnimStep_{0};
    uint32_t animBannerMillis_{0};
};

} // namespace HardwareDriver
