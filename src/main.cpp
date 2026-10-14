#include <Arduino.h>
#include <Preferences.h>
#include "SynthEngine.h"
#include "MidiParser.h"
#include "Presets.h"

// =========================================================================
// ESP32-S3 Rev 2.0 (Akai MPK Mini MK4 Native USB Host, BLE SysEx, OLED, PCM5102A)
// =========================================================================
#include "esp32s3_driver.h"
#include "knob_page.h"
#include "oled_ui.h"
#include "system_status.h"
#include "metro_led.h"
#include "ble_midi_server.h"
#include "usb_host_midi.h"
#include "animations/AnimationRegistry.h"
#include "profiling.h"
#include "loudness_demo.h"
#include "animations/KeyboardAnimation.h"
#include "animations/SynthwaveAnimation.h"
#include "animations/LissajousAnimation.h"
#include "animations/RoadRacerAnimation.h"
#include "animations/ChoirAnimation.h"
#include "animations/PondAnimation.h"
#include "animations/PlinkoAnimation.h"
#include "animations/TypewriterAnimation.h"
#include "animations/KnitAnimation.h"
#include "animations/WashAnimation.h"
#include "animations/SeismoAnimation.h"

static Synth::SynthEngine                g_synthEngine;
static HardwareDriver::OledDisplayUI    g_oledUi;
static HardwareDriver::BleMidiServer    g_bleMidi;
static HardwareDriver::UsbHostMidi      g_usbMidi;
static HardwareDriver::SystemStatus     g_status;   // boot / connection faults shown on the OLED

// OLED Animation System
static HardwareDriver::AnimationRegistry  g_animRegistry;
static HardwareDriver::KeyboardAnimation     g_animKeyboard;
static HardwareDriver::SynthwaveAnimation    g_animSynthwave;
static HardwareDriver::LissajousAnimation    g_animLissajous;
static HardwareDriver::RoadRacerAnimation    g_animRacer;
// MIDI toys (prototyped in docs/oled_midi_toys.html)
static HardwareDriver::ChoirAnimation        g_animChoir;
static HardwareDriver::PondAnimation         g_animPond;
static HardwareDriver::PlinkoAnimation       g_animPlinko;
static HardwareDriver::TypewriterAnimation   g_animTypewriter;
static HardwareDriver::KnitAnimation         g_animKnit;
static HardwareDriver::WashAnimation         g_animWash;
static HardwareDriver::SeismoAnimation       g_animSeismo;

static uint32_t g_lastOledFrameMillis = 0;
static HardwareDriver::MetroLed g_metroLed;
static HardwareDriver::TapLed   g_tapLed;
constexpr uint32_t OLED_FRAME_INTERVAL_MS = 50; // ~20 FPS (prevents I2C bus starvation)

// Device settings kept across power cycles (Rhythm page R8 "Output": stereo / mono)
constexpr const char* SETTINGS_NVS_NAMESPACE = "settings";
static bool g_savedMonoOutput = false;

static void loadDeviceSettings() {
    Preferences prefs;
    if (!prefs.begin(SETTINGS_NVS_NAMESPACE, true)) return; // first boot: namespace not created yet
    g_savedMonoOutput = prefs.getBool("mono_out", false);
    prefs.end();
    g_synthEngine.setMonoOutput(g_savedMonoOutput);
}

// Writes only on change, so turning the knob back and forth costs one flash write per flip
static void saveDeviceSettingsIfChanged() {
    const bool mono = g_synthEngine.isMonoOutput();
    if (mono == g_savedMonoOutput) return;
    Preferences prefs;
    if (!prefs.begin(SETTINGS_NVS_NAMESPACE, false)) return;
    prefs.putBool("mono_out", mono);
    prefs.end();
    g_savedMonoOutput = mono;
    Serial.printf("[Settings] Output: %s\n", mono ? "MONO (left)" : "STEREO");
}

void setup() {
    setCpuFrequencyMhz(240); // Lock ESP32-S3 Xtensa dual cores to full 240 MHz
    Serial.begin(115200);
    delay(200);

    Serial.println("==================================================");
    Serial.println("  ESP32-S3 Polyphonic Synthesizer — Rev 2.0      ");
    Serial.println("  Hardware Controller: Akai MPK Mini MK4         ");
    Serial.println("==================================================");
    Serial.printf("[System] CPU: %u MHz | Main Loop Core: %d\n", getCpuFrequencyMhz(), xPortGetCoreID());

    using HardwareDriver::SystemFault;
    if (!psramFound()) {
        g_status.set(SystemFault::Psram, millis());
    }

    // 1. Initialize Core 1 Dedicated Real-Time Audio DSP & I2S DMA
    if (!HardwareDriver::initHardware(g_synthEngine)) {
        Serial.println("[Fatal] Failed to initialize I2S DAC / Audio Core!");
        g_status.set(HardwareDriver::getAudioInitError() == HardwareDriver::AudioInitError::AudioTask
                         ? SystemFault::AudioTask : SystemFault::AudioDac, millis());
    }

    // 2. Initialize 0.96\" SSD1306 OLED (I2C: GPIO 8 SDA, GPIO 9 SCL)
    if (!g_oledUi.init()) {
        Serial.println("[Warning] Failed to initialize SSD1306 OLED");
    }

    // 2b. Register OLED animations and connect to display
    HardwareDriver::IAnimation* const animations[] = {
        &g_animKeyboard,    // Default: MK4 Keyboard
        &g_animSynthwave,   // Synthwave sunset
        &g_animLissajous,   // Oscilloscope Lissajous
        &g_animRacer,       // Road Racer
        &g_animChoir, &g_animPond, &g_animPlinko, &g_animTypewriter, &g_animKnit,
        &g_animWash, &g_animSeismo,
    };
    for (HardwareDriver::IAnimation* anim : animations) {
        anim->init();
        g_animRegistry.registerAnimation(anim);
    }
    g_oledUi.setAnimationRegistry(&g_animRegistry);
    g_oledUi.setSystemStatus(&g_status);
    Serial.printf("[OLED] %u animations registered\n", static_cast<unsigned>(g_animRegistry.count()));

    // 2c. Onboard RGB LED flashes in time with the metronome
    g_metroLed.init();

    // 3. Set Master Volume to 85% default
    g_synthEngine.setMasterVolume(0.85f);

    // 4. Initialize BLE Server for SysEx patch updates & NVS user patch persistence
    g_bleMidi.init(g_synthEngine);
    if (!g_bleMidi.isNvsOk()) g_status.set(SystemFault::Nvs, millis());
    if (!g_bleMidi.isBleOk()) g_status.set(SystemFault::Ble, millis());
    loadDeviceSettings();
    if (g_savedMonoOutput) Serial.println("[Settings] Output: MONO (left)");

    // 5. USB Host Stack (Akai MPK Mini MK4 Native USB)
    g_usbMidi.init(g_synthEngine);
    g_usbMidi.setOnStatusHud([](const char* label, uint8_t val, const char* text) {
        g_oledUi.notifyKnobTouched(label, val, text);
    });
    g_usbMidi.setOnPresetChanged([](uint8_t idx, const char* name) {
        g_oledUi.setPreset(idx, name);
    });
    g_usbMidi.setOnPageChanged([](HardwareDriver::KnobPage page) {
        g_oledUi.setPage(page);
    });
    g_usbMidi.setOnAnimationStep([](int step) {
        g_oledUi.requestAnimationStep(step);
    });
    g_usbMidi.selectPreset(0);

    Serial.println(">>> System Initialized (Rev 2.0 Ready, MPK Mini MK4 USB Host) <<<");
}

void loop() {
    // -------------------------------------------------------------
    // Core 0 Main Control Loop (USB Host MPK, BLE SysEx, UI)
    // -------------------------------------------------------------
    g_usbMidi.poll();
    g_bleMidi.poll();
#ifdef SYNTH_LOUDNESS_DEMO
    static LoudnessDemo::Player loudnessDemo;
    loudnessDemo.poll(g_synthEngine, [](uint8_t idx) { g_usbMidi.selectPreset(idx); });
#endif

    uint32_t now = millis();
    g_metroLed.update(g_synthEngine, now);
    g_tapLed.update(g_synthEngine, g_usbMidi, now);
    if (now - g_lastOledFrameMillis >= OLED_FRAME_INTERVAL_MS) {
        g_lastOledFrameMillis = now;
        // Fault watchers (cheap flag reads; the OLED shows whatever they set)
        g_status.updateMpkLink(now, g_usbMidi.isDeviceConnected(), g_usbMidi.getMpkSysExCount());
        if (!g_bleMidi.isNvsOk()) g_status.set(HardwareDriver::SystemFault::Nvs, now);
        g_oledUi.setKeybedNoteOffset(g_usbMidi.getKeybedNoteOffset());
        saveDeviceSettingsIfChanged();
        PROFILE_OLED_BEGIN();
        g_oledUi.render(g_synthEngine, g_synthEngine.getMasterVolume());
        PROFILE_OLED_END(g_animRegistry.getActiveName());
    }

    static uint32_t lastHeartbeat = 0;
    if (now - lastHeartbeat >= 1000) {
        lastHeartbeat = now;
        static uint32_t loggedLostOffs = 0, loggedLostOns = 0;
        const uint32_t lostOffs = g_usbMidi.getLostNoteOffCount();
        const uint32_t lostOns = g_usbMidi.getLostNoteOnCount();
        if (lostOffs != loggedLostOffs || lostOns != loggedLostOns) {
            loggedLostOffs = lostOffs;
            loggedLostOns = lostOns;
            Serial.printf("[MIDI] Lost keybed events: note-offs %u, note-ons %u\n",
                          (unsigned)lostOffs, (unsigned)lostOns);
        }
        uint8_t count = 0;
        uint8_t notes[8] = {0};
        float envs[8] = {0};
        g_synthEngine.getVoiceSnapshot(notes, envs, count);
        if (count > 0) {
            Serial.printf("[Voice Status] Active: %u voice(s) | SustainPedal: %d | FreeHeap: %u | ",
                          count, g_synthEngine.isSustainPedalHeld(), ESP.getFreeHeap());
            for (int v = 0; v < 8; ++v) {
                if (envs[v] > 0.001f) {
                    Serial.printf("[V%d: note %u env %.2f] ", v, notes[v], envs[v]);
                }
            }
            Serial.println();
        }
    }

    yield();
}
