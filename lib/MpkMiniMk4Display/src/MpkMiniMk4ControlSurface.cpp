#include "MpkMiniMk4ControlSurface.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
#include <Arduino.h> // MOCK_HAL: tests/mock_hal clock, which tests can pin with setMockMillis()
static inline uint32_t surface_millis() {
    return millis();
}
#else
static inline uint32_t surface_millis() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
}
#endif


namespace MpkMiniMk4 {

ControlSurface::ControlSurface()
    : displayDriver_(&ownedDisplayDriver_) {
    bankAValues_.fill(0);
    bankBValues_.fill(0);
}

bool ControlSurface::init(Synth::SynthEngine& engine, DisplayDriver* displayDriver) {
    engine_ = &engine;
    if (displayDriver) {
        displayDriver_ = displayDriver;
    } else {
        displayDriver_ = &ownedDisplayDriver_;
    }

    const auto& p = engine_->getCurrentPreset();
    for (size_t k = 0; k < 8; ++k) {
        bankAValues_[k] = p.bankA.defaults[k];
        bankBValues_[k] = p.bankB.defaults[k];
    }

    return true;
}

void ControlSurface::setDisplayDriver(DisplayDriver* displayDriver) {
    if (displayDriver) {
        displayDriver_ = displayDriver;
    } else {
        displayDriver_ = &ownedDisplayDriver_;
    }
}

void ControlSurface::setPluginPortSender(SendPacketFn fn) {
    pluginPortSender_ = fn;
    displayDriver_->setPluginPortSender(fn);
    if (engine_ && isPluginPortConnected()) {
        displayDriver_->selectHardwareBank(isBankBActive_);
        syncHardwareKnobs();
        updateOledDisplay();
    }
}

void ControlSurface::setSoftwareControlPortSender(SendPacketFn fn) {
    swControlPortSender_ = fn;
    displayDriver_->setSoftwareControlPortSender(fn);
    if (engine_ && isSoftwareControlPortConnected()) {
        updateOledDisplay();
    }
}

SurfaceStatus ControlSurface::processMidiMessage(uint8_t status, uint8_t data1, uint8_t data2) {
    lastUiEvent_ = UiEvent::NONE;
    if (!engine_) return SurfaceStatus::Ignored;
    tick();

    // 1. Real-Time Messages
    if (status >= 0xF8) {
        if (status == 0xF8) {
            engine_->midiClockTick();
        } else if (status == 0xFA || status == 0xFB) {
            engine_->midiClockStart();
        } else if (status == 0xFC) {
            engine_->midiClockStop();
        }
        return SurfaceStatus::Success;
    }

    uint8_t msgType = status & 0xF0;

    switch (msgType) {
        case 0x90: // Note On
            if (data2 > 0) {
                if (isShiftButtonHeld_) {
                    // SHIFT + Keybed Note: Musical Root Key Selection (C..B)
                    uint8_t rootKey = data1 % 12;
                    engine_->setRootKey(rootKey);
                    if (statusHudCb_) {
                        char keyBuf[16];
                        std::snprintf(keyBuf, sizeof(keyBuf), "KEY: %s", engine_->getRootKeyName());
                        statusHudCb_(keyBuf, static_cast<uint8_t>((rootKey * 127) / 11), nullptr);
                    }
                    lastUiEvent_ = UiEvent::ROOT_KEY_SET;
                } else {
                    engine_->noteOn(data1, data2);
                    lastUiEvent_ = UiEvent::NOTE_ON;
                }
            } else {
                engine_->noteOff(data1);
                lastUiEvent_ = UiEvent::NOTE_OFF;
            }
            return SurfaceStatus::Success;

        case 0x80: // Note Off
            engine_->noteOff(data1);
            lastUiEvent_ = UiEvent::NOTE_OFF;
            return SurfaceStatus::Success;

        case 0xB0: // Control Change
            return handleControlChange(data1, data2);

        case 0xE0: { // Pitch Bend
            int16_t bendVal = static_cast<int16_t>((static_cast<int>(data2) << 7) | data1);
            engine_->pitchBend(bendVal);
            lastUiEvent_ = UiEvent::PITCH_BEND;
            return SurfaceStatus::Success;
        }

        case 0xC0: // Program Change
            selectPreset(data1 % static_cast<uint8_t>(Synth::SynthModel::Count));
            lastUiEvent_ = UiEvent::PRESET_CHANGED;
            return SurfaceStatus::Success;

        default:
            return SurfaceStatus::Ignored;
    }
}

bool ControlSurface::processSysEx(const uint8_t* data, size_t len) {
    if (displayDriver_) {
        return displayDriver_->handleSysEx(data, len);
    }
    return false;
}

bool ControlSurface::sendKeepalive() {
    tick();
    if (displayDriver_) {
        return displayDriver_->sendKeepalive();
    }
    return false;
}

void ControlSurface::flushTransientHud(uint32_t nowMs, bool force) {
    if (!hudPending_ || !displayDriver_) return;
    if (!force && nowMs - lastHudWriteMs_ < HUD_MIN_WRITE_INTERVAL_MS) return;
    if (pendingHudIsHome_) {
        displayDriver_->showHomeScreen(pendingHudHeader_, pendingHudLabel_, pendingHudTheme_);
    } else {
        displayDriver_->showKnobHud(pendingHudHeader_, pendingHudLabel_, pendingHudValue_, pendingHudValue7_,
                                    pendingHudTheme_);
    }
    lastHudWriteMs_ = nowMs;
    hudPending_     = displayDriver_->hasDeferredHostDraw(); // held-back pixels go out one write interval later
}

void ControlSurface::queueTransientHud(const std::string& header, const std::string& label,
                                       const std::string& value, const Theme& theme, bool isHome, int value7Bit) {
    hudActive_        = !isHome; // the synth-name home screen stays up; only knob overlays expire
    lastHudTime_      = surface_millis();
    pendingHudHeader_ = header;
    pendingHudLabel_  = label;
    pendingHudValue_  = value;
    pendingHudTheme_  = theme;
    pendingHudIsHome_ = isHome;
    pendingHudValue7_ = value7Bit;
    hudPending_       = true;
    flushTransientHud(lastHudTime_, !displayDriver_->isOwnershipLocked());
}

// Synth-name home screen for the native-screen path: stays up until a knob overlay or a built-in MK4 menu replaces it
void ControlSurface::showPresetHud() {
    const auto& p = engine_->getCurrentPreset();
    char infoBuf[24];
    if (rhythmPageActive_) {
        std::snprintf(infoBuf, sizeof(infoBuf), "RHYTHM %d %s", static_cast<int>(engine_->getMetronomeBpm()),
                      engine_->getTimeSignatureString());
    } else {
        std::snprintf(infoBuf, sizeof(infoBuf), "%u/%u | BANK %c", static_cast<unsigned>(currentPresetIndex_ + 1),
                      static_cast<unsigned>(Synth::SynthModel::Count), isBankBActive_ ? 'B' : 'A');
    }
    auto theme = DisplayDriver::themeForPresetName(p.name ? p.name : "SYNTH", isBankBActive_);
    queueTransientHud(p.name ? p.name : "SYNTH", infoBuf, "", theme, true);
}

// The MK4 takes the screen back for its own UI (built-in menus, +/- program view). Keep asking for it back:
// the device answers 19 00 11 as soon as no menu is open, and onDeviceResume redraws the home screen.
void ControlSurface::pollNativeUi(uint32_t nowMs) {
    if (!displayDriver_ || displayDriver_->isScreenTakeoverEnabled() || !isPluginPortConnected()) return;
    bool open = displayDriver_->isNativeUiOpen();
    if (open && !wasNativeUiOpen_) nativeProbeDueMs_ = nowMs + 300;
    wasNativeUiOpen_ = open;
    if (open && nowMs >= nativeProbeDueMs_) {
        displayDriver_->probeScreen();
        nativeProbeDueMs_ = nowMs + 1000;
    }
    if (displayDriver_->consumeNeedsRedraw() && engine_) {
        wasNativeUiOpen_ = false;
        showPresetHud();
    }
}

void ControlSurface::tick(uint32_t nowMs) {
    if (nowMs == 0) nowMs = surface_millis();
    pollNativeUi(nowMs);
    if (hudPending_) flushTransientHud(nowMs, false);
    if (hudActive_ && nowMs >= lastHudTime_ && (nowMs - lastHudTime_ >= hudHoldTimeMs_)) {
        updateOledDisplay(); // clears hudActive_ and, without takeover, returns the screen to the MK4 firmware
    }
}

int ControlSurface::resolveHardwareKnobIndex(uint8_t ccNumber) const {
    if (ccNumber >= Synth::CC_HW_KNOB_1 && ccNumber <= Synth::CC_HW_KNOB_8) {
        return static_cast<int>(ccNumber - Synth::CC_HW_KNOB_BASE);
    }
    return -1;
}

SurfaceStatus ControlSurface::handleControlChange(uint8_t ccNumber, uint8_t ccValue) {
    // 1. Check if this is one of the 8 physical rotary encoders K1..K8 (CC 24..31)
    int knobIdx = resolveHardwareKnobIndex(ccNumber);
    if (knobIdx >= 0 && knobIdx < 8) {
        // Enforce Plugin Port connection requirement
        if (!isPluginPortConnected()) {
            if (errorCb_) {
                errorCb_("CONNECT MPK PLUGIN PORT: Knobs halted without SysEx 0x12 pre-seeding");
            }
            return SurfaceStatus::ErrorPluginPortNotConnected;
        }

        const auto& preset = engine_->getCurrentPreset();
        uint8_t targetCc;
        const char* label;
        if (rhythmPageActive_) {
            if (RHYTHM_PAGE_KNOBS[knobIdx].cc == RHYTHM_KNOB_UNUSED) return SurfaceStatus::Ignored;
            rhythmValues_[knobIdx] = ccValue;
            targetCc = RHYTHM_PAGE_KNOBS[knobIdx].cc;
            label    = RHYTHM_PAGE_KNOBS[knobIdx].label;
        } else {
            // Update live dual-bank session cache
            (isBankBActive_ ? bankBValues_ : bankAValues_)[knobIdx] = ccValue;
            const auto& activeBank = isBankBActive_ ? preset.bankB : preset.bankA;
            targetCc = activeBank.ccs[knobIdx];
            label    = activeBank.labels[knobIdx];
        }

        // Apply parameter mutation to synth engine
        engine_->controlChange(targetCc, ccValue);
        if (targetCc == Synth::CC_TEMPO_BPM) syncDeviceTempo();

        std::string hudLabel = label ? label : "Param";
        std::string hudValue;
        const bool described = engine_->describeParameter(targetCc, ccValue, hudLabel, hudValue);
        if (!described) hudValue = std::to_string(static_cast<unsigned>(ccValue));

        lastUiEvent_   = UiEvent::KNOB_TURNED;
        lastKnobIndex_ = static_cast<uint8_t>(knobIdx);
        lastKnobValue_ = ccValue;
        lastTargetCc_  = targetCc;

        hudActive_   = true;
        lastHudTime_ = surface_millis();

        // Echo Suppression: Do NOT send SysEx 0x12 back on physical knob turns!
        if (displayDriver_ && isPluginPortConnected()) {
            auto theme = DisplayDriver::themeForPresetName(preset.name ? preset.name : "SYNTH", isBankBActive_);
            // Knob name + value + bar; back to the home screen after the hold time (tick()).
            // Value updates are rate-limited (a sweep emits dozens of CCs); tick() flushes the latest.
            queueTransientHud(preset.name ? preset.name : "SYNTH", hudLabel, hudValue, theme, false, ccValue);
        }

        if (statusHudCb_) {
            char hudBuf[24];
            if (rhythmPageActive_) {
                std::snprintf(hudBuf, sizeof(hudBuf), "R%u %s", static_cast<unsigned>(knobIdx + 1), label);
            } else {
                std::snprintf(hudBuf, sizeof(hudBuf), "K%u %s",
                              static_cast<unsigned>(isBankBActive_ ? (knobIdx + 9) : (knobIdx + 1)),
                              label ? label : "Param");
            }
            // Rhythm values (time signature, key...) are names, not amounts: show the text on the OLED
            statusHudCb_(hudBuf, ccValue, (rhythmPageActive_ && described) ? hudValue.c_str() : nullptr);
        }

        return SurfaceStatus::Success;
    }

    // 2. Momentary SHIFT Button (CC 17 on the MK4; CC 18 / CC 102 for custom maps). Knob fine-adjust is done by the MK4 firmware.
    if (ccNumber == Synth::CC_HW_SHIFT_BUTTON || ccNumber == Synth::CC_HW_CUSTOM_SHIFT || ccNumber == Synth::CC_HW_SHIFT_TOGGLE_2) {
        setShiftButtonHeld(ccValue >= 64);
        lastUiEvent_ = UiEvent::SHIFT_CHANGED;
        return SurfaceStatus::Success;
    }
    // CC 74 is the DAW transport button, not SHIFT (RE doc §6.1): swallowed so it can't reach Filter Cutoff
    if (ccNumber == Synth::CC_HW_DAW_TRANSPORT_BTN) {
        return SurfaceStatus::Ignored;
    }

    // 3. BANK A/B button (CC 12 on the MK4, CC 50 alternate):
    //    - press (>= 64, then 0 on release) toggles the latched bank
    //    - SHIFT + press opens / closes the Rhythm page; a plain press closes it
    //    - a 0/1 toggle switch sets Bank B on 1 and Bank A on 0 (or on a release held over 400 ms)
    if (ccNumber == Synth::CC_HW_BANK_AB_BUTTON || ccNumber == Synth::CC_HW_BANK_AB_ALT) {
        uint32_t now = surface_millis();
        if (ccValue >= 64) {
            lastBankAbWasLowToggle_ = false;
            lastBankAbPressTime_ = now;
            if (isShiftButtonHeld_) {
                setRhythmPageActive(!rhythmPageActive_);
            } else if (rhythmPageActive_) {
                setRhythmPageActive(false); // back to the A/B bank that was open before
            } else {
                setBankBLatched(!isBankBLatched_);
            }
            lastUiEvent_ = UiEvent::PAGE_CHANGED;
        } else if (ccValue > 0) {
            lastBankAbWasLowToggle_ = true;
            setBankBLatched(true);
            lastUiEvent_ = UiEvent::PAGE_CHANGED;
        } else if (lastBankAbWasLowToggle_ || (now - lastBankAbPressTime_ > 400)) {
            if (isBankBLatched_ && !rhythmPageActive_) {
                setBankBLatched(false);
                lastUiEvent_ = UiEvent::PAGE_CHANGED;
            }
        }
        return SurfaceStatus::Success;
    }

    // 3b. Foot switch (CC 86): each press toggles Bank A / Bank B
    if (ccNumber == Synth::CC_HW_FOOTSWITCH_BANK) {
        if (ccValue >= 64) {
            setBankBLatched(!isBankBLatched_);
            lastUiEvent_ = UiEvent::PAGE_CHANGED;
        }
        return SurfaceStatus::Success;
    }

    // 4. Custom-map Bank Left / Right buttons (CC 16 -> Bank A, CC 52 -> Bank B)
    if (ccNumber == Synth::CC_HW_CUSTOM_BANK_LEFT || ccNumber == Synth::CC_HW_CUSTOM_BANK_RIGHT) {
        if (ccValue > 0) {
            setBankBLatched(ccNumber == Synth::CC_HW_CUSTOM_BANK_RIGHT);
            lastUiEvent_ = UiEvent::PAGE_CHANGED;
        }
        return SurfaceStatus::Success;
    }

    // 5. '-' / '+' buttons (CC 80 / CC 81): previous / next preset; with SHIFT, previous / next OLED animation
    if (ccNumber == Synth::CC_HW_PRESET_PREV || ccNumber == Synth::CC_HW_PRESET_NEXT) {
        if (ccValue > 0) {
            const int dir = (ccNumber == Synth::CC_HW_PRESET_NEXT) ? 1 : -1;
            if (isShiftButtonHeld_) {
                if (animationStepCb_) animationStepCb_(dir);
                lastUiEvent_ = dir > 0 ? UiEvent::ANIM_NEXT : UiEvent::ANIM_PREV;
            } else {
                stepPreset(dir);
                lastUiEvent_ = dir > 0 ? UiEvent::PRESET_NEXT : UiEvent::PRESET_PREV;
            }
        }
        return SurfaceStatus::Success;
    }

    // 5b. Preset navigation from other MIDI sources: Bank Select (CC 0 / 32) picks a slot, cursor and
    //     transport keys (CC 110 / 114, CC 111 / 115) step through the slots
    if (ccNumber == Synth::CC_BANK_SELECT_MSB || ccNumber == Synth::CC_BANK_SELECT_LSB) {
        selectPreset(ccValue % static_cast<uint8_t>(Synth::SynthModel::Count));
        lastUiEvent_ = UiEvent::PRESET_CHANGED;
        return SurfaceStatus::Success;
    }
    if (ccNumber == Synth::CC_HW_CURSOR_LEFT || ccNumber == Synth::CC_HW_TRANSPORT_PREV ||
        ccNumber == Synth::CC_HW_CURSOR_RIGHT || ccNumber == Synth::CC_HW_TRANSPORT_NEXT) {
        if (ccValue > 0) {
            const bool next = (ccNumber == Synth::CC_HW_CURSOR_RIGHT || ccNumber == Synth::CC_HW_TRANSPORT_NEXT);
            stepPreset(next ? 1 : -1);
            lastUiEvent_ = next ? UiEvent::PRESET_NEXT : UiEvent::PRESET_PREV;
        }
        return SurfaceStatus::Success;
    }

    // 6. Push-turn encoder rotation (CC 72 on the MK4, CC 14 custom map)
    if (ccNumber == Synth::CC_HW_CUSTOM_ENCODER || ccNumber == Synth::CC_HW_NAV_ENCODER) {
        // Relative encoder (RE doc §5.3): CW = 1..63, CCW = 128-n (127..65)
        int step = (ccValue <= 63) ? static_cast<int>(ccValue) : static_cast<int>(ccValue) - 128;
        if (step == 0) return SurfaceStatus::Success;
        step = std::clamp(step, -4, 4);
        if (isShiftButtonHeld_) step *= 4;
        stepPreset(step);
        lastUiEvent_ = UiEvent::PRESET_ENCODER;
        return SurfaceStatus::Success;
    }

    // 7. Push-turn encoder click (CC 13 on the MK4, CC 15 custom map): show the preset screen again
    if (ccNumber == Synth::CC_HW_PUSH || ccNumber == Synth::CC_HW_CUSTOM_PUSH) {
        if (ccValue >= 64) updateOledDisplay();
        return SurfaceStatus::Success;
    }

    // 8. Custom-map metronome button (CC 19 or CC 118)
    if (ccNumber == Synth::CC_HW_CUSTOM_METRO_BTN || ccNumber == Synth::CC_METRONOME_TOGGLE) {
        if (ccValue >= 64) {
            engine_->toggleMetronome();
            reportMetronomeState();
        }
        return SurfaceStatus::Success;
    }

    // 9. TAP TEMPO (CC 82 on the MK4; CC 20 / CC 119 custom maps). SHIFT + TAP toggles the metronome
    //    (SHIFT+TAP arrives as CC 17 held, then CC 82)
    if (ccNumber == Synth::CC_HW_TAP_TEMPO || ccNumber == Synth::CC_HW_CUSTOM_TAP_BTN || ccNumber == Synth::CC_TAP_TEMPO) {
        if (ccValue >= 64) {
            if (isShiftButtonHeld_) {
                engine_->toggleMetronome();
                reportMetronomeState();
            } else {
                engine_->tapTempo(surface_millis());
                refreshRhythmKnobs();
                syncDeviceTempo();
                if (statusHudCb_) {
                    char bpmBuf[16];
                    std::snprintf(bpmBuf, sizeof(bpmBuf), "BPM: %d", static_cast<int>(engine_->getMetronomeBpm()));
                    statusHudCb_(bpmBuf, static_cast<uint8_t>(((engine_->getMetronomeBpm() - 40.0f) * 127.0f) / 200.0f), nullptr);
                }
                lastUiEvent_ = UiEvent::TAP_TEMPO;
            }
        }
        return SurfaceStatus::Success;
    }

    // 10. Modulation wheel: synth vibrato, plus a HUD on the synth's OLED
    if (ccNumber == Synth::CC_MOD_WHEEL) {
        engine_->controlChange(ccNumber, ccValue);
        if (statusHudCb_) statusHudCb_("MOD WHEEL", ccValue, nullptr);
        return SurfaceStatus::Success;
    }

    // 11. Everything else (sustain pedal, All Notes Off, ...) goes to the engine
    engine_->controlChange(ccNumber, ccValue);
    return SurfaceStatus::Success;
}

void ControlSurface::reportMetronomeState() {
    const bool on = engine_->isMetronomeEnabled();
    if (statusHudCb_) statusHudCb_(on ? "METRO: ON" : "METRO: OFF", on ? 127 : 0, nullptr);
    lastUiEvent_ = UiEvent::METRO_TOGGLED;
}

void ControlSurface::selectPreset(uint8_t presetIndex) {
    if (presetIndex >= static_cast<uint8_t>(Synth::SynthModel::Count)) {
        presetIndex = 0;
    }
    currentPresetIndex_ = presetIndex;
    isBankBLatched_     = false;
    isBankBActive_      = false;
    rhythmPageActive_   = false;

    if (engine_) {
        engine_->setShiftActive(false);
        engine_->loadPresetIndex(currentPresetIndex_);
        const auto& p = engine_->getCurrentPreset();
        for (size_t k = 0; k < 8; ++k) {
            bankAValues_[k] = p.bankA.defaults[k];
            bankBValues_[k] = p.bankB.defaults[k];
        }

        if (presetChangedCb_) {
            presetChangedCb_(currentPresetIndex_, p.name ? p.name : "PATCH");
        }
    }

    if (displayDriver_) {
        displayDriver_->selectHardwareBank(false);
    }

    if (displayDriver_ && engine_ && isPluginPortConnected() && !displayDriver_->isScreenTakeoverEnabled()) {
        showPresetHud(); // updates the overlay in place; a release + re-claim here would flash the native screen
    } else {
        updateOledDisplay();
    }
    syncHardwareKnobs();
}

void ControlSurface::stepPreset(int step) {
    int totalPresets = static_cast<int>(Synth::SynthModel::Count);
    int newPreset    = static_cast<int>(currentPresetIndex_) + step;
    while (newPreset < 0) newPreset += totalPresets;
    newPreset %= totalPresets;
    selectPreset(static_cast<uint8_t>(newPreset));
}

void ControlSurface::setShiftButtonHeld(bool held) {
    isShiftButtonHeld_ = held;
}

void ControlSurface::setBankBLatched(bool latched) {
    const bool leftRhythm = rhythmPageActive_;
    rhythmPageActive_ = false;
    isBankBLatched_   = latched;
    if (leftRhythm && latched == isBankBActive_) {
        // Bank unchanged, so syncEffectiveBankState() won't redraw: restore the bank's knobs here
        updateOledDisplay();
        syncHardwareKnobs();
        return;
    }
    syncEffectiveBankState();
}

void ControlSurface::setRhythmPageActive(bool active) {
    if (rhythmPageActive_ == active) return;
    rhythmPageActive_ = active;
    if (active && engine_) {
        for (size_t k = 0; k < RHYTHM_PAGE_KNOBS.size(); ++k) {
            rhythmValues_[k] = rhythmKnobValueFromEngine(RHYTHM_PAGE_KNOBS[k].cc);
        }
    }
    updateOledDisplay();
    syncHardwareKnobs();
}

void ControlSurface::refreshRhythmKnobs() {
    if (!rhythmPageActive_ || !engine_) return;
    for (size_t k = 0; k < RHYTHM_PAGE_KNOBS.size(); ++k) {
        rhythmValues_[k] = rhythmKnobValueFromEngine(RHYTHM_PAGE_KNOBS[k].cc);
    }
    syncHardwareKnobs();
}

void ControlSurface::syncDeviceTempo() {
    if (!engine_ || !displayDriver_ || !isPluginPortConnected()) return;
    displayDriver_->setDeviceTempo(static_cast<uint16_t>(std::lround(engine_->getMetronomeBpm())));
}

// Inverse of SynthEngine::controlChange for the rhythm CCs, so a knob picks up from the current setting
uint8_t ControlSurface::rhythmKnobValueFromEngine(uint8_t cc) const {
    auto norm = [](float x) { return static_cast<uint8_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 127.0f)); };
    switch (cc) {
        case Synth::CC_METRO_VOLUME: {
            if (!engine_->isMetronomeEnabled()) return 0; // 0 = off; turning it up starts the metronome
            uint8_t v = norm(engine_->getMetronomeVolume());
            return v == 0 ? 1 : v;
        }
        case Synth::CC_TEMPO_BPM:
            return norm((engine_->getMetronomeBpm() - 40.0f) / 200.0f);
        case Synth::CC_TIME_SIGNATURE: {
            // Same order as Synth::TimeSignaturePreset
            static constexpr uint8_t sigs[][2] = {{4, 4}, {3, 4}, {2, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};
            constexpr unsigned count = static_cast<unsigned>(Synth::TimeSignaturePreset::Count);
            static_assert(sizeof(sigs) / sizeof(sigs[0]) == count, "time signature table out of sync");
            unsigned idx = 0;
            for (unsigned i = 0; i < count; ++i) {
                if (sigs[i][0] == engine_->getTimeSigBeatsPerBar() && sigs[i][1] == engine_->getTimeSigBeatUnit()) {
                    idx = i;
                    break;
                }
            }
            return static_cast<uint8_t>((idx * 128 + 64) / count); // middle of the preset's CC range
        }
        case Synth::CC_TIME_SIG_BEATS:
            return static_cast<uint8_t>(((engine_->getTimeSigBeatsPerBar() - 1) * 127 + 14) / 15);
        case Synth::CC_METRO_CLICK: {
            const unsigned step = engine_->isMetronomeKeyTuned() ? engine_->getRootKey() + 1u : 0u;
            return static_cast<uint8_t>((step * 128 + 64) / 13); // middle of the step's CC range
        }
        case Synth::CC_OUTPUT_MODE:
            return engine_->isMonoOutput() ? 127 : 0;
        case Synth::CC_METRO_SILENT:
            return engine_->isMetronomeSilent() ? 127 : 0;
        case Synth::CC_VOLUME:
            return norm(engine_->getMasterVolume());
        default:
            return 0;
    }
}

void ControlSurface::syncEffectiveBankState() {
    // SHIFT never switches bank: the MK4 firmware divides knob deltas by 4 while SHIFT is held (fine adjust)
    bool newBankB = isBankBLatched_;
    if (newBankB != isBankBActive_) {
        isBankBActive_ = newBankB;
        if (engine_) {
            engine_->setShiftActive(isBankBActive_);
        }
        if (displayDriver_) {
            displayDriver_->selectHardwareBank(isBankBActive_);
        }
        updateOledDisplay();
        syncHardwareKnobs();
    }
}

void ControlSurface::syncHardwareKnobs() {
    if (!isPluginPortConnected() || !engine_) return;

    const auto& vals = rhythmPageActive_ ? rhythmValues_ : (isBankBActive_ ? bankBValues_ : bankAValues_);
    displayDriver_->syncKnobValues(vals);
}

void ControlSurface::updateOledDisplay() {
    hudActive_  = false;
    hudPending_ = false;
    if (!engine_ || !displayDriver_) return;
    if (!displayDriver_->isScreenTakeoverEnabled()) {
        if (isPluginPortConnected()) showPresetHud(); // back to the synth-name home screen
        else displayDriver_->endTransientHud();
        return;
    }

    const auto& p          = engine_->getCurrentPreset();
    const auto& bank       = isBankBActive_ ? p.bankB : p.bankA;
    std::string progHeader = std::string(p.name ? p.name : "SYNTH") +
                             (rhythmPageActive_ ? " [R]" : (isBankBActive_ ? " [B]" : " [A]"));
    auto theme             = DisplayDriver::themeForPresetName(p.name ? p.name : "SYNTH", isBankBActive_);

    // 1. Sync 284-byte Active RAM preset on Software Control Port ONLY when Plugin Port is NOT connected!
    // (When Plugin Port is connected, sending 284 bytes resets the hardware out of Plugin Mode and resets knob accumulators)
    if (isSoftwareControlPortConnected() && !isPluginPortConnected()) {
        std::array<std::string, 8> knobLabels;
        for (size_t k = 0; k < 8; ++k) {
            const char* l = rhythmPageActive_ ? RHYTHM_PAGE_KNOBS[k].label : bank.labels[k];
            knobLabels[k] = l ? l : "";
        }
        displayDriver_->syncRamPreset(progHeader, knobLabels, theme.padColor);
    }

    // 2. Same home screen as the native-screen path, drawn immediately
    if (isPluginPortConnected()) {
        showPresetHud();
        flushTransientHud(surface_millis(), true);
    }
}

void ControlSurface::onExternalParameterChanged(uint8_t ccNumber, uint8_t value7Bit) {
    if (!engine_) return;
    uint8_t val = (value7Bit > 127) ? 127 : value7Bit;

    const auto& p = engine_->getCurrentPreset();

    if (rhythmPageActive_) {
        for (size_t k = 0; k < RHYTHM_PAGE_KNOBS.size(); ++k) {
            if (RHYTHM_PAGE_KNOBS[k].cc == ccNumber) {
                rhythmValues_[k] = val;
                if (isPluginPortConnected()) displayDriver_->setKnobValue(static_cast<uint8_t>(k), val);
            }
        }
    }

    // Check Bank A
    for (size_t k = 0; k < 8; ++k) {
        if (p.bankA.ccs[k] == ccNumber) {
            bankAValues_[k] = val;
            if (!isBankBActive_ && !rhythmPageActive_ && isPluginPortConnected()) {
                displayDriver_->setKnobValue(static_cast<uint8_t>(k), val);
            }
            return;
        }
    }

    // Check Bank B
    for (size_t k = 0; k < 8; ++k) {
        if (p.bankB.ccs[k] == ccNumber) {
            bankBValues_[k] = val;
            if (isBankBActive_ && !rhythmPageActive_ && isPluginPortConnected()) {
                displayDriver_->setKnobValue(static_cast<uint8_t>(k), val);
            }
            return;
        }
    }
}

} // namespace MpkMiniMk4
