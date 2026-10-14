#pragma once

#include <Arduino.h>
#include "SynthEngine.h"
#include "usb_host_midi.h"

namespace HardwareDriver {

// Flashes the DevKitC-1 onboard WS2812 RGB LED (RGB_BUILTIN, GPIO 48) on every metronome beat:
// red on the downbeat, green on the other beats. Driven from loop(), never from the audio task,
// because each LED write is a blocking RMT transfer. The LED is only written when its color changes.
class MetroLed {
public:
    static constexpr uint32_t FLASH_MS = 70;

    void init() { write(0, 0, 0); }

    void update(const Synth::SynthEngine& engine, uint32_t nowMs) {
        const uint32_t clicks = engine.getMetroClickCount();
        if (clicks != lastClickCount_) {
            lastClickCount_ = clicks;
            flashStartMs_ = nowMs;
            lit_ = true;
            if (engine.isLastMetroClickDownbeat()) write(BRIGHTNESS, 0, 0);
            else                                   write(0, BRIGHTNESS, 0);
        } else if (lit_ && (nowMs - flashStartMs_ >= FLASH_MS || !engine.isMetronomeEnabled())) {
            lit_ = false;
            write(0, 0, 0);
        }
    }

private:
    static constexpr uint8_t BRIGHTNESS = 64;

    static void write(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_BUILTIN
        neopixelWrite(RGB_BUILTIN, r, g, b);
#endif
    }

    uint32_t lastClickCount_{0};
    uint32_t flashStartMs_{0};
    bool     lit_{false};
};

// Flashes the MK4's TAP TEMPO button LED on every metronome beat, audible or silent, so the beat can be seen
// while recording with the click muted. Beat 1 is full brightness, the other beats half. Driven from loop():
// two USB-MIDI messages per beat, no logging.
class TapLed {
public:
    static constexpr uint32_t FLASH_MS = 90;

    void update(const Synth::SynthEngine& engine, UsbHostMidi& usb, uint32_t nowMs) {
        if (!usb.isDeviceConnected()) {
            lit_ = false; // the MK4 resets its LEDs when it reconnects
            lastClickCount_ = engine.getMetroClickCount();
            return;
        }
        const uint32_t clicks = engine.getMetroClickCount();
        if (clicks != lastClickCount_) {
            lastClickCount_ = clicks;
            flashStartMs_ = nowMs;
            lit_ = true;
            usb.setButtonLed(Synth::CC_HW_TAP_TEMPO, engine.isLastMetroClickDownbeat() ? CH_FULL : CH_HALF, true);
        } else if (lit_ && (nowMs - flashStartMs_ >= FLASH_MS || !engine.isMetronomeEnabled())) {
            lit_ = false;
            usb.setButtonLed(Synth::CC_HW_TAP_TEMPO, CH_FULL, false);
        }
    }

private:
    static constexpr uint8_t CH_FULL = 6; // MIDI channel 7: solid 100 %
    static constexpr uint8_t CH_HALF = 2; // MIDI channel 3: solid 50 %

    uint32_t lastClickCount_{0};
    uint32_t flashStartMs_{0};
    bool     lit_{false};
};

} // namespace HardwareDriver
