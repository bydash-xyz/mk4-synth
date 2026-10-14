#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <functional>
#include "MpkMiniMk4Display.h"
#include "SynthEngine.h"
#include "Presets.h"
#include "MidiMapping.h"

namespace MpkMiniMk4 {

enum class SurfaceStatus : uint8_t {
    Success                     = 0,
    Ignored                     = 1,
    ErrorPluginPortNotConnected = 2,
};

enum class ChangeSource : uint8_t {
    PhysicalKnob            = 0,
    ExternalPresetLoad      = 1,
    ExternalBankSwitch      = 2,
    ExternalParameterChange = 3,
};

// What the last MIDI message did; read with ControlSurface::getLastUiEvent()
enum class UiEvent : uint8_t {
    NONE           = 0,
    PRESET_CHANGED = 1,  // Program Change or a direct select
    PRESET_PREV    = 2,  // '-' button
    PRESET_NEXT    = 3,  // '+' button
    PRESET_ENCODER = 4,  // Push-turn encoder rotation
    KNOB_TURNED    = 5,  // K1..K8
    SHIFT_CHANGED  = 6,  // SHIFT held / released
    PAGE_CHANGED   = 7,  // Bank A, Bank B or Rhythm page
    NOTE_ON        = 8,
    NOTE_OFF       = 9,
    ROOT_KEY_SET   = 10, // SHIFT + keybed note
    METRO_TOGGLED  = 11,
    TAP_TEMPO      = 12,
    PITCH_BEND     = 13,
    ANIM_PREV      = 14, // SHIFT + '-'
    ANIM_NEXT      = 15  // SHIFT + '+'
};

struct RhythmKnob {
    uint8_t     cc;
    const char* label;
};

// Rhythm page slot with no function (no MIDI CC is 0xFF): the knob does nothing
inline constexpr uint8_t RHYTHM_KNOB_UNUSED = 0xFF;

// Knob page opened with SHIFT + BANK: metronome, tempo, time signature, click sound, output mode, silent
// metronome, volume.
// Scales are left to the MK4's own Scale mode; the root key is set by the Click knob or SHIFT + a key.
inline constexpr std::array<RhythmKnob, 8> RHYTHM_PAGE_KNOBS{{
    {Synth::CC_METRO_VOLUME,   "Metro Level"},
    {Synth::CC_TEMPO_BPM,      "Tempo"},
    {Synth::CC_TIME_SIGNATURE, "Time Sig"},
    {Synth::CC_TIME_SIG_BEATS, "Beats/Bar"},
    {Synth::CC_METRO_CLICK,    "Click"},
    {Synth::CC_OUTPUT_MODE,    "Output"},
    {Synth::CC_METRO_SILENT,   "Metro Out"},
    {Synth::CC_VOLUME,         "Master Vol"},
}};

class ControlSurface {
public:
    using SendPacketFn         = DisplayDriver::SendPacketFn;
    using PresetChangeCallback = std::function<void(uint8_t index, const char* name)>;
    // valueText: formatted value (e.g. "6/8"), or nullptr to show the raw 0..127 value
    using StatusHudCallback    = std::function<void(const char* label, uint8_t value, const char* valueText)>;
    using ErrorCallback        = std::function<void(const char* errorMessage)>;
    using AnimationStepCallback = std::function<void(int step)>;

    ControlSurface();
    ~ControlSurface() = default;

    bool init(Synth::SynthEngine& engine, DisplayDriver* displayDriver = nullptr);

    // Transport injection (Plugin Port is mandatory for parameter control)
    void setPluginPortSender(SendPacketFn fn);
    void setSoftwareControlPortSender(SendPacketFn fn);
    bool isPluginPortConnected() const { return static_cast<bool>(pluginPortSender_); }
    bool isSoftwareControlPortConnected() const { return static_cast<bool>(swControlPortSender_); }

    // Event callbacks
    void setOnPresetChanged(PresetChangeCallback cb) { presetChangedCb_ = std::move(cb); }
    void setOnStatusHud(StatusHudCallback cb)       { statusHudCb_     = std::move(cb); }
    void setOnError(ErrorCallback cb)               { errorCb_         = std::move(cb); }
    // SHIFT + '-' / '+' steps the synth's OLED animation (-1 / +1)
    void setOnAnimationStep(AnimationStepCallback cb) { animationStepCb_ = std::move(cb); }

    // Inbound MIDI from the MK4: the one place that maps its keys, knobs and buttons to the synth
    SurfaceStatus processMidiMessage(uint8_t status, uint8_t data1, uint8_t data2);
    UiEvent getLastUiEvent() const { return lastUiEvent_; }

    // Inbound SysEx processor (e.g. Heartbeat Ping CMD 0x19)
    bool processSysEx(const uint8_t* data, size_t len);

    // Physical-keybed shift (octave/transpose) reported by the MPK; see DisplayDriver.
    uint8_t getKeybedOctave() const { return displayDriver_ ? displayDriver_->getKeybedOctave() : 4; }
    int getKeybedTranspose() const { return displayDriver_ ? displayDriver_->getKeybedTranspose() : 0; }
    int getKeybedNoteOffset() const { return displayDriver_ ? displayDriver_->getKeybedNoteOffset() : 0; }
    bool requestKeybedState() { return displayDriver_ && displayDriver_->requestKeybedState(); }

    // Proactively refresh controller's screen owner lease timer
    bool sendKeepalive();

    // Periodic processing (e.g. parameter HUD auto-dismiss timer)
    void tick(uint32_t nowMs = 0);
    bool isHudActive() const { return hudActive_; }
    uint32_t getLastHudTime() const { return lastHudTime_; }

    // Preset & Bank management
    void selectPreset(uint8_t presetIndex);
    void stepPreset(int step);
    void setShiftButtonHeld(bool held);
    void setBankBLatched(bool latched);
    void setRhythmPageActive(bool active);
    // Re-reads the rhythm knob values from the engine (e.g. after tap tempo) and re-seeds the hardware
    void refreshRhythmKnobs();
    // Pushes the synth tempo to the MK4 so its TAP LED / arp follow the metronome
    void syncDeviceTempo();

    // External parameter updates (e.g. from a patch received over BLE SysEx)
    void onExternalParameterChanged(uint8_t ccNumber, uint8_t value7Bit);

    // Getters for state
    uint8_t getCurrentPreset() const { return currentPresetIndex_; }
    bool isBankBActive() const { return isBankBActive_; }
    bool isShiftButtonHeld() const { return isShiftButtonHeld_; }
    bool isBankBLatched() const { return isBankBLatched_; }
    bool isRhythmPageActive() const { return rhythmPageActive_; }

    uint8_t getLastKnobIndex() const { return lastKnobIndex_; }
    uint8_t getLastKnobValue() const { return lastKnobValue_; }
    uint8_t getLastTargetCc() const { return lastTargetCc_; }

    DisplayDriver& getDisplayDriver() { return *displayDriver_; }
    const DisplayDriver& getDisplayDriver() const { return *displayDriver_; }
    void setDisplayDriver(DisplayDriver* displayDriver);

    void setScreenTakeoverEnabled(bool enabled) { if (displayDriver_) displayDriver_->setScreenTakeoverEnabled(enabled); }
    bool isScreenTakeoverEnabled() const { return displayDriver_ ? displayDriver_->isScreenTakeoverEnabled() : false; }

    const std::array<uint8_t, 8>& getBankAValues() const { return bankAValues_; }
    const std::array<uint8_t, 8>& getRhythmValues() const { return rhythmValues_; }

    void updateOledDisplay();
    void syncHardwareKnobs();

private:
    void flushTransientHud(uint32_t nowMs, bool force);
    // isHome: header = synth name, label = info line; otherwise a knob screen with value7Bit driving the value bar
    void queueTransientHud(const std::string& header, const std::string& label, const std::string& value,
                           const Theme& theme, bool isHome = false, int value7Bit = -1);
    void showPresetHud();
    void reportMetronomeState();
    void pollNativeUi(uint32_t nowMs);
    void syncEffectiveBankState();
    uint8_t rhythmKnobValueFromEngine(uint8_t cc) const;
    int resolveHardwareKnobIndex(uint8_t ccNumber) const;
    SurfaceStatus handleControlChange(uint8_t ccNumber, uint8_t ccValue);

    Synth::SynthEngine* engine_{nullptr};
    DisplayDriver       ownedDisplayDriver_{};
    DisplayDriver*      displayDriver_{&ownedDisplayDriver_};

    SendPacketFn        pluginPortSender_{};
    SendPacketFn        swControlPortSender_{};

    PresetChangeCallback presetChangedCb_{};
    StatusHudCallback    statusHudCb_{};
    ErrorCallback        errorCb_{};
    AnimationStepCallback animationStepCb_{};

    uint8_t currentPresetIndex_{0};
    bool    isBankBActive_{false};
    bool    isShiftButtonHeld_{false};
    bool    isBankBLatched_{false};
    bool    rhythmPageActive_{false};
    bool    lastBankAbWasLowToggle_{false};
    uint32_t lastBankAbPressTime_{0};

    UiEvent lastUiEvent_{UiEvent::NONE};
    uint8_t lastKnobIndex_{0};
    uint8_t lastKnobValue_{0};
    uint8_t lastTargetCc_{0};
    bool    hudActive_{false};
    uint32_t lastHudTime_{0};
    uint32_t hudHoldTimeMs_{1800};

    // Rate-limited screen updates (home screen and knob HUD, with or without screen takeover)
    static constexpr uint32_t HUD_MIN_WRITE_INTERVAL_MS = 60;
    bool        hudPending_{false};
    bool        wasNativeUiOpen_{false};
    uint32_t    nativeProbeDueMs_{0};
    uint32_t    lastHudWriteMs_{0};
    std::string pendingHudHeader_, pendingHudLabel_, pendingHudValue_;
    Theme       pendingHudTheme_{};
    bool        pendingHudIsHome_{false};
    int         pendingHudValue7_{-1};

    std::array<uint8_t, 8> bankAValues_{};
    std::array<uint8_t, 8> bankBValues_{};
    std::array<uint8_t, 8> rhythmValues_{};
};

} // namespace MpkMiniMk4
