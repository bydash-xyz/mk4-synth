#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include "Arduino.h" // mock HAL clock (ScopedFrozenMillis)
#include "SynthEngine.h"
#include "Presets.h"
#include "MpkMiniMk4ControlSurface.h"

using namespace MpkMiniMk4;

void test_plugin_port_required() {
    std::cout << "[Test] Running Plugin Port Connection Requirement test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    // 1. Without Plugin Port sender, isPluginPortConnected() is false
    assert(!surface.isPluginPortConnected());

    std::string lastError;
    surface.setOnError([&](const char* msg) {
        lastError = msg;
    });

    // 2. Physical knob turn must be rejected and raise error
    auto status = surface.processMidiMessage(0xB0, 24, 100);
    assert(status == SurfaceStatus::ErrorPluginPortNotConnected);
    assert(lastError.find("PLUGIN PORT") != std::string::npos);

    // 3. Connect Plugin Port sender
    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });
    assert(surface.isPluginPortConnected());

    // 4. Now knob turn succeeds
    status = surface.processMidiMessage(0xB0, 24, 100);
    assert(status == SurfaceStatus::Success);

    std::cout << "  -> Plugin Port Connection Requirement tests PASSED!" << std::endl;
}

void test_preset_load_pre_seeds_8_knobs() {
    std::cout << "[Test] Running Preset Load Hardware Pre-Seeding test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });
    pluginPackets.clear();

    // Select Preset 0 (Juno-60)
    surface.selectPreset(0);


    // Filter pluginPackets for SysEx 0x12 packets (10 bytes, command 0x12)
    std::vector<std::vector<uint8_t>> knobPackets;
    for (const auto& pkt : pluginPackets) {
        if (pkt.size() == 10 && pkt[4] == 0x12) {
            knobPackets.push_back(pkt);
        }
    }

    // Must have exactly 8 knob pre-seed packets for Bank A
    assert(knobPackets.size() == 8);

    const auto& juno = Synth::getPresets()[0];
    for (size_t k = 0; k < 8; ++k) {
        assert(knobPackets[k][7] == k); // Knob index 0..7
        assert(knobPackets[k][8] == juno.bankA.defaults[k]); // Initial preset parameter value
        assert(knobPackets[k][9] == 0xF7);
    }

    std::cout << "  -> Preset Load Hardware Pre-Seeding tests PASSED!" << std::endl;
}

void test_physical_knob_turn_and_echo_suppression() {
    std::cout << "[Test] Running Physical Knob Turn & Echo Suppression test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    surface.selectPreset(0);
    pluginPackets.clear();

    // Turn K1 (CC 24) to 115
    auto status = surface.processMidiMessage(0xB0, 24, 115);
    assert(status == SurfaceStatus::Success);

    // Verify SynthEngine received parameter update
    assert(surface.getLastKnobIndex() == 0);
    assert(surface.getLastKnobValue() == 115);
    assert(surface.getLastTargetCc() == Synth::CC_FILTER_CUTOFF);

    // Verify ECHO SUPPRESSION: pluginPackets must NOT contain 0x12 for K1
    for (const auto& pkt : pluginPackets) {
        if (pkt.size() == 10 && pkt[4] == 0x12) {
            assert(false && "Echo suppression failed: SysEx 0x12 was echoed on physical knob turn!");
        }
    }

    std::cout << "  -> Physical Knob Turn & Echo Suppression tests PASSED!" << std::endl;
}

void test_bank_switch_preserves_tweaked_values() {
    std::cout << "[Test] Running Bank Switch Dual-Bank Session Persistence test..." << std::endl;
    ScopedFrozenMillis frozenClock; // BANK A/B press + release must not look like a 400 ms hold
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    surface.selectPreset(0);

    // 1. In Bank A, tweak Knob 1 (CC 24) to 115
    surface.processMidiMessage(0xB0, 24, 115);

    // 2. Switch to Bank B (CC 17 = Bank Right)
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_BANK_RIGHT, 127);
    assert(surface.isBankBActive());

    // Verify 8 packets pre-seeded Bank B defaults
    std::vector<std::vector<uint8_t>> bankBPackets;
    bool foundBankBSysEx = false;
    for (const auto& pkt : pluginPackets) {
        if (pkt.size() == 10 && pkt[4] == 0x12) {
            bankBPackets.push_back(pkt);
        } else if (pkt.size() == 9 && pkt[4] == 0x3A && pkt[7] == 0x00) {
            foundBankBSysEx = true;
        }
    }
    assert(bankBPackets.size() == 8);
    assert(foundBankBSysEx);
    const auto& juno = Synth::getPresets()[0];
    for (size_t k = 0; k < 8; ++k) {
        assert(bankBPackets[k][7] == k);
        assert(bankBPackets[k][8] == juno.bankB.defaults[k]);
    }

    // 3. In Bank B, tweak Knob 1 (CC 24, which is K9) to 42
    surface.processMidiMessage(0xB0, 24, 42);

    // 4. Switch back to Bank A (CC 16 = Bank Left)
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_BANK_LEFT, 127);
    assert(!surface.isBankBActive());

    // Verify 8 packets pre-seeded Bank A, and Knob 0 restored to 115 (NOT default!)
    std::vector<std::vector<uint8_t>> bankAPackets;
    bool foundBankASysEx = false;
    for (const auto& pkt : pluginPackets) {
        if (pkt.size() == 10 && pkt[4] == 0x12) {
            bankAPackets.push_back(pkt);
        } else if (pkt.size() == 9 && pkt[4] == 0x3A && pkt[7] == 0x01) {
            foundBankASysEx = true;
        }
    }
    assert(bankAPackets.size() == 8);
    assert(foundBankASysEx);
    assert(bankAPackets[0][7] == 0);
    assert(bankAPackets[0][8] == 115); // Preserved tweaked value!

    // 5. Switch to Bank B again -> Knob 0 restored to 42!
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_BANK_RIGHT, 127);
    assert(surface.isBankBActive());

    bankBPackets.clear();
    for (const auto& pkt : pluginPackets) {
        if (pkt.size() == 10 && pkt[4] == 0x12) {
            bankBPackets.push_back(pkt);
        }
    }
    assert(bankBPackets[0][7] == 0);
    assert(bankBPackets[0][8] == 42); // Preserved tweaked Bank B value!

    // 6. Test physical hardware BANK A/B button (CC 12)
    // 1st press (127) toggles to Bank A
    surface.processMidiMessage(0xB0, 12, 127);
    assert(!surface.isBankBActive());
    surface.processMidiMessage(0xB0, 12, 0); // Release keeps Bank A
    assert(!surface.isBankBActive());

    // 2nd press (127) toggles back to Bank B
    surface.processMidiMessage(0xB0, 12, 127);
    assert(surface.isBankBActive());
    surface.processMidiMessage(0xB0, 12, 0);
    assert(surface.isBankBActive());

    // 7. Verify alternate CC 50 works identically
    surface.processMidiMessage(0xB0, 50, 127);
    assert(!surface.isBankBActive());
    surface.processMidiMessage(0xB0, 50, 0);
    assert(!surface.isBankBActive());

    std::cout << "  -> Bank Switch Dual-Bank Session Persistence tests PASSED!" << std::endl;
}

void test_external_parameter_change_pre_seeds_hardware() {
    std::cout << "[Test] Running External Parameter Change Pre-Seeding test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    surface.selectPreset(0); // Juno-60, Bank A active: K1 is Cutoff (CC 74)
    pluginPackets.clear();

    // External change on Cutoff (e.g. from a BLE patch)
    surface.onExternalParameterChanged(Synth::CC_FILTER_CUTOFF, 122);

    // Must emit exactly 1 SysEx 0x12 targeting Knob 0 with value 122
    std::vector<std::vector<uint8_t>> knobPackets;
    for (const auto& pkt : pluginPackets) {
        if (pkt.size() == 10 && pkt[4] == 0x12) {
            knobPackets.push_back(pkt);
        }
    }
    assert(knobPackets.size() == 1);
    assert(knobPackets[0][7] == 0);   // K1
    assert(knobPackets[0][8] == 122); // Value 122

    std::cout << "  -> External Parameter Change Pre-Seeding tests PASSED!" << std::endl;
}

void test_compound_gestures() {
    std::cout << "[Test] Running Compound Gestures test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    surface.selectPreset(2); // Prophet-5

    // 1. Hold SHIFT (CC 18)
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_SHIFT, 127);
    assert(surface.isShiftButtonHeld());

    // 2. SHIFT + Keybed Note 66 (F#4 -> root key 6 = F#)
    surface.processMidiMessage(0x90, 66, 100);
    assert(engine.getRootKey() == 6);
    assert(std::strcmp(engine.getRootKeyName(), "F#") == 0);
    assert(engine.getActiveVoiceCount() == 0); // No voice note triggered!

    // 3. SHIFT + Nav Dial (CC 14 or CC 72) -> Jump by 4 presets
    surface.processMidiMessage(0xB0, Synth::CC_HW_NAV_ENCODER, 1);
    assert(surface.getCurrentPreset() == 6); // 2 + 4 = 6

    // 4. Release SHIFT
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_SHIFT, 0);
    assert(!surface.isShiftButtonHeld());

    std::cout << "  -> Compound Gestures tests PASSED!" << std::endl;
}

void test_screen_owner_heartbeat_keepalive() {
    std::cout << "[Test] Running Screen Owner Heartbeat & Keepalive test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    // 1. Proactive Keepalive
    pluginPackets.clear();
    bool keepaliveSent = surface.sendKeepalive();
    assert(keepaliveSent);
    assert(pluginPackets.size() == 1);
    assert(pluginPackets[0].size() == 25);
    assert(pluginPackets[0][0] == 0xF0);
    assert(pluginPackets[0][1] == 0x47);
    assert(pluginPackets[0][3] == 0x5D);
    assert(pluginPackets[0][4] == 0x19); // CMD 0x19 (Keepalive Pong)
    assert(pluginPackets[0][6] == 0x11); // Length 17
    assert(pluginPackets[0][7] == 0x01); // OwnerType::PluginSVK0
    assert(pluginPackets[0][24] == 0xF7);

    // 2. Reactive Ping -> Pong (Hardware emits 0x19 ping every 2.0s)
    pluginPackets.clear();
    const uint8_t ping[8] = {0xF0, 0x47, 0x00, 0x5D, 0x19, 0x00, 0x00, 0xF7};
    bool pingHandled = surface.processSysEx(ping, sizeof(ping));
    assert(pingHandled);
    assert(pluginPackets.size() == 1);
    assert(pluginPackets[0].size() == 25);
    assert(pluginPackets[0][4] == 0x19);
    assert(pluginPackets[0][7] == 0x01);

    // 3. Ignore unrelated SysEx
    const uint8_t other[5] = {0xF0, 0x7E, 0x7F, 0x06, 0xF7};
    bool otherHandled = surface.processSysEx(other, sizeof(other));
    assert(!otherHandled);

    std::cout << "  -> Screen Owner Heartbeat & Keepalive tests PASSED!" << std::endl;
}

void test_shift_button_cc17_and_hud_auto_dismiss() {
    std::cout << "[Test] Running Physical SHIFT (CC 17) & Parameter HUD Auto-Dismiss test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    surface.selectPreset(0); // Juno-60
    assert(!surface.isShiftButtonHeld());
    assert(!surface.isBankBActive());
    assert(!surface.isHudActive());

    // 1. Press and hold physical SHIFT button (CC 17 = 127).
    // SHIFT = firmware fine-adjust (delta/4), NOT a bank switch: no bank change, no SysEx traffic.
    std::string hudLabel;
    surface.setOnStatusHud([&](const char* label, uint8_t, const char*) { hudLabel = label; });
    size_t packetsBefore = pluginPackets.size();
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 127);
    assert(surface.isShiftButtonHeld());
    assert(!surface.isBankBActive());
    assert(pluginPackets.size() == packetsBefore);

    // 2. Turn Knob K1 with SHIFT: still Bank A K1 (Cutoff), HUD unchanged
    surface.processMidiMessage(0xB0, 24, 61);
    assert(surface.isHudActive());
    assert(surface.getLastTargetCc() == Synth::CC_FILTER_CUTOFF);
    assert(hudLabel.rfind("K1", 0) == 0);

    // 3. Release SHIFT: bank stays A, no bank-sync SysEx
    packetsBefore = pluginPackets.size();
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 0);
    assert(!surface.isShiftButtonHeld());
    assert(!surface.isBankBActive());
    assert(pluginPackets.size() == packetsBefore);
    surface.processMidiMessage(0xB0, 24, 62);
    assert(hudLabel.rfind("K1", 0) == 0);

    // 3b. CC 74 is the DAW transport button, not SHIFT
    surface.processMidiMessage(0xB0, Synth::CC_HW_DAW_TRANSPORT_BTN, 127);
    assert(!surface.isShiftButtonHeld());

    // 3c. Push encoder is relative: CW n -> +n, CCW 128-n -> -n (clamped to 4)
    surface.selectPreset(2);
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_ENCODER, 2);
    assert(surface.getCurrentPreset() == 4);
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_ENCODER, 127);
    assert(surface.getCurrentPreset() == 3);
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_ENCODER, 126);
    assert(surface.getCurrentPreset() == 1);
    // SHIFT held x4 persists across repeated ticks (preset change must not clear it)
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_ENCODER, 1);
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_ENCODER, 1);
    assert(surface.getCurrentPreset() == 9 % static_cast<int>(Synth::SynthModel::Count));
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 0);
    surface.selectPreset(0);

    // 4. Test HUD auto-dismiss without shift
    // Turn Knob K1 in Bank A (targets Cutoff)
    surface.processMidiMessage(0xB0, 24, 75);
    assert(surface.isHudActive());

    uint32_t t = surface.getLastHudTime();
    // Advance tick by 500ms -> HUD still active
    surface.tick(t + 500);
    assert(surface.isHudActive());

    // Advance tick beyond hudHoldTimeMs (1800ms) -> HUD dismissed automatically
    surface.tick(t + 1900);
    assert(!surface.isHudActive());

    std::cout << "  -> Physical SHIFT (CC 17) & Parameter HUD Auto-Dismiss tests PASSED!" << std::endl;
}

void test_knob_hud_without_takeover() {
    std::cout << "[Test] Running knob name/value HUD with screen takeover disabled..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);
    surface.setScreenTakeoverEnabled(false); // ESP32 default: native MK4 screen preserved

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });
    surface.selectPreset(0); // synth-name home screen
    std::this_thread::sleep_for(std::chrono::milliseconds(80)); // let the HUD write rate limit pass

    auto settle = [&]() { // real-time waits: the HUD write rate limit runs on the wall clock
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        surface.tick();
    };
    settle(); // flush the pending home screen

    pluginPackets.clear();
    surface.processMidiMessage(0xB0, 24, 75); // K1 = Cutoff
    assert(surface.isHudActive());
    settle();
    assert(surface.getDisplayDriver().hasDeferredHostDraw()); // value held back until the MK4 repainted line 2
    settle();
    assert(!surface.getDisplayDriver().hasDeferredHostDraw());
    assert(!pluginPackets.empty());
    const auto& lines = surface.getDisplayDriver().getLastLines();
    assert(!lines[1].text.empty());
    assert(lines[2].text.empty()); // value is host-drawn, the MK4 line only provides the background
    assert(surface.getDisplayDriver().getHostValueText().find("Hz") != std::string::npos);
    assert(surface.getDisplayDriver().isOwnershipLocked());

    // Further turns must not touch the MK4 text lines (every 0x10/0x14 clears and redraws the line = flicker):
    // identical value = no packets, new value = only 0x71 blits (changed value pixels + bar delta)
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, 24, 75);
    settle();
    assert(pluginPackets.empty());
    surface.processMidiMessage(0xB0, 24, 80);
    assert(pluginPackets.empty()); // rate-limited: held until the next tick past the write interval
    settle();
    assert(!pluginPackets.empty());
    for (const auto& pkt : pluginPackets) assert(pkt[4] == 0x71);
    assert(surface.getDisplayDriver().getLastLines()[1].text.rfind("K1", 0) != 0); // no K-number prefix

    // After the hold time the screen goes back to the synth name (it is not released to the native screen)
    pluginPackets.clear();
    surface.tick(surface.getLastHudTime() + 1900);
    settle();
    assert(!surface.isHudActive());
    assert(surface.getDisplayDriver().isOwnershipLocked());
    assert(surface.getDisplayDriver().getLastLines()[1].text.rfind("1/", 0) == 0); // "1/32 | BANK A"
    assert(surface.getDisplayDriver().getHostValueText().empty());

    std::cout << "  -> Knob HUD without takeover PASSED!" << std::endl;
}

void test_preset_name_hud_and_native_menu_yield() {
    std::cout << "[Test] Running always-on synth name and native MK4 menu (SHIFT+Pad8 / +/-) yield..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);
    surface.setScreenTakeoverEnabled(false);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    auto settle = [&]() { std::this_thread::sleep_for(std::chrono::milliseconds(80)); surface.tick(); };

    // Preset change shows the synth name and keeps it (no release after the hold time)
    surface.selectPreset(1);
    settle();
    const auto& drv = surface.getDisplayDriver();
    assert(drv.isOwnershipLocked());
    assert(drv.getLastLines()[0].text == std::string(engine.getCurrentPreset().name));
    surface.tick(surface.getLastHudTime() + 5000);
    assert(drv.isOwnershipLocked());

    // Device takes the screen back (built-in menu or its own +/- program view): F0 47 00 5D 19 00 01 00 F7
    const uint8_t suspend[] = {0xF0, 0x47, 0x00, 0x5D, 0x19, 0x00, 0x01, 0x00, 0xF7};
    surface.processSysEx(suspend, sizeof(suspend));
    assert(drv.isNativeUiOpen());

    // While it is open, knobs and preset changes must not draw
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, 24, 90);
    surface.selectPreset(3);
    for (const auto& pkt : pluginPackets) assert(pkt.size() < 5 || (pkt[4] != 0x10 && pkt[4] != 0x14 && pkt[4] != 0x1C));

    // We keep asking for the screen back (0x1C claim); the device says a menu is still open
    uint32_t t = surface.getLastHudTime() + 10000;
    pluginPackets.clear();
    surface.tick(t);          // first probe is due by now
    assert(pluginPackets.size() == 1 && pluginPackets[0][4] == 0x1C);
    surface.processSysEx(suspend, sizeof(suspend));
    assert(drv.isNativeUiOpen());

    // Menu closes: device re-announces ownership (25-byte form) and we redraw the synth name
    pluginPackets.clear();
    const uint8_t resume[] = {0xF0, 0x47, 0x00, 0x5D, 0x19, 0x00, 0x11, 0x01,
                              'B','O','L','D','-','C','A','R','S','O','N','-','S','V','K','0', 0xF7};
    surface.processSysEx(resume, sizeof(resume));
    assert(!drv.isNativeUiOpen());
    surface.tick(t + 500);
    settle();
    assert(!pluginPackets.empty());
    assert(drv.isOwnershipLocked());
    assert(drv.getLastLines()[0].text == std::string(engine.getCurrentPreset().name));

    std::cout << "  -> Always-on synth name and native menu yield PASSED!" << std::endl;
}

void test_physical_tap_tempo_cc82() {
    std::cout << "[Test] Running Physical TAP TEMPO (CC 82) & SHIFT+TAP Metronome test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });

    std::vector<std::string> hudLabels;
    surface.setOnStatusHud([&](const char* label, uint8_t, const char*) {
        hudLabels.emplace_back(label);
    });

    surface.selectPreset(0);
    engine.setMetronomeBpm(60.0f);
    const bool metroInitially = engine.isMetronomeEnabled();

    // 1. Three press/release taps 500 ms apart -> 120 BPM, metronome state untouched.
    //    Release (value 0) must be ignored, and CC 82 must not leak through as a synth parameter.
    //    The mock clock is stepped instead of sleeping: a busy CI runner can oversleep by 100+ ms.
    {
        ScopedFrozenMillis frozenClock;
        for (int i = 0; i < 3; ++i) {
            setMockMillis(10000 + 500 * i);
            assert(surface.processMidiMessage(0xB0, Synth::CC_HW_TAP_TEMPO, 127) == SurfaceStatus::Success);
            assert(surface.processMidiMessage(0xB0, Synth::CC_HW_TAP_TEMPO, 0) == SurfaceStatus::Success);
        }
    }
    const float bpm = engine.getMetronomeBpm();
    assert(bpm > 119.0f && bpm < 121.0f);
    assert(engine.isMetronomeEnabled() == metroInitially);
    assert(!hudLabels.empty() && hudLabels.back().rfind("BPM:", 0) == 0);

    // 2. SHIFT (CC 17 held) + CC 82 -> metronome toggle, BPM unchanged
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 127);
    assert(surface.isShiftButtonHeld());
    surface.processMidiMessage(0xB0, Synth::CC_HW_TAP_TEMPO, 127);
    assert(engine.isMetronomeEnabled() == !metroInitially);
    assert(hudLabels.back().rfind("METRO:", 0) == 0);
    surface.processMidiMessage(0xB0, Synth::CC_HW_TAP_TEMPO, 0);
    assert(engine.isMetronomeEnabled() == !metroInitially); // release does not toggle back
    assert(engine.getMetronomeBpm() == bpm);

    // 3. SHIFT+TAP again toggles back
    surface.processMidiMessage(0xB0, Synth::CC_HW_TAP_TEMPO, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_TAP_TEMPO, 0);
    assert(engine.isMetronomeEnabled() == metroInitially);
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 0);
    assert(!surface.isShiftButtonHeld());

    std::cout << "  -> Physical TAP TEMPO (CC 82) & SHIFT+TAP Metronome tests PASSED!" << std::endl;
}

void test_shift_bank_opens_rhythm_page() {
    std::cout << "[Test] Running SHIFT+BANK Rhythm Page (tempo / time signature) test..." << std::endl;
    ScopedFrozenMillis frozenClock; // BANK A/B press + release must not look like a 400 ms hold
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });
    auto knobSeeds = [&]() {
        std::vector<uint8_t> vals;
        for (const auto& pkt : pluginPackets) {
            if (pkt.size() == 10 && pkt[4] == 0x12) vals.push_back(pkt[8]);
        }
        return vals;
    };

    surface.selectPreset(0);
    engine.setMetronomeBpm(90.0f);
    engine.setTimeSignature(3, 4);

    // Plain BANK -> Bank B
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 0);
    assert(surface.isBankBActive() && !surface.isRhythmPageActive());

    // 1. SHIFT + BANK opens the Rhythm page; knobs are seeded from the engine's current tempo / meter
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 0);
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 0);
    assert(surface.isRhythmPageActive());
    assert(surface.isBankBActive()); // underlying bank is kept
    auto seeds = knobSeeds();
    assert(seeds.size() == 8);
    assert(seeds[1] == 32);  // 90 BPM
    assert(seeds[2] == 24);  // 3/4 preset slot
    assert(seeds[3] == 17);  // 3 beats per bar

    // Seeded values round-trip through the engine without changing anything
    for (size_t k = 0; k < 8; ++k) {
        surface.processMidiMessage(0xB0, static_cast<uint8_t>(Synth::CC_HW_KNOB_1 + k), seeds[k]);
    }
    assert(engine.getTimeSigBeatsPerBar() == 3 && engine.getTimeSigBeatUnit() == 4);
    assert(static_cast<int>(engine.getMetronomeBpm() + 0.5f) == 90);

    // 2. Knobs now drive tempo and time signature; the OLED gets the value as text, not 0..127
    std::string hudText;
    surface.setOnStatusHud([&](const char*, uint8_t, const char* text) { hudText = text ? text : ""; });
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_3, 72); // 6/8 slot
    assert(surface.getLastTargetCc() == Synth::CC_TIME_SIGNATURE);
    assert(engine.getTimeSigBeatsPerBar() == 6 && engine.getTimeSigBeatUnit() == 8);
    assert(hudText == "6/8");
    std::string label, value;
    assert(engine.describeParameter(Synth::CC_TIME_SIGNATURE, 72, label, value) && value == "6/8");

    // Tempo also goes to the MK4 (SysEx 0x6C) so its TAP LED follows the metronome
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_2, 127);
    assert(engine.getMetronomeBpm() == 240.0f);
    assert(hudText == "240 BPM");
    const std::vector<uint8_t> setTempo240 = {0xF0, 0x47, 0x00, 0x5D, 0x6C, 0x00, 0x02, 0x01, 0x70, 0xF7};
    bool sentTempo = false;
    for (const auto& pkt : pluginPackets) sentTempo |= (pkt == setTempo240);
    assert(sentTempo);

    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_4, 127); // 16 beats, keeps the /8 unit
    assert(engine.getTimeSigBeatsPerBar() == 16 && engine.getTimeSigBeatUnit() == 8);
    assert(hudText == "16/8");

    // R5 is one dial for the metronome click: WOOD, then a click tuned to C..B (which sets the Root Key)
    engine.setMetronomeKeyTuned(false);
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_5, 0);
    assert(surface.getLastTargetCc() == Synth::CC_METRO_CLICK);
    assert(!engine.isMetronomeKeyTuned() && hudText == "WOOD");
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_5, 15); // step 1 = C
    assert(engine.isMetronomeKeyTuned() && engine.getRootKey() == 0 && hudText == "C");
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_5, 127); // step 12 = B
    assert(engine.isMetronomeKeyTuned() && engine.getRootKey() == 11 && hudText == "B");
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_5, 60); // step 6 = F
    assert(engine.getRootKey() == 5);

    // R6 switches the output between stereo and mono-on-left (for one TRS cable into a balanced input)
    assert(!engine.isMonoOutput());
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_6, 100);
    assert(surface.getLastTargetCc() == Synth::CC_OUTPUT_MODE);
    assert(engine.isMonoOutput() && hudText == "MONO L");
    {
        // Dry preset (L == R in stereo): mono puts the whole mix on the left and silences the right
        Synth::SynthEngine dry;
        dry.init(44100);
        dry.loadPreset(Synth::SynthModel::FM_Clavinet);
        dry.setMonoOutput(true);
        dry.noteOn(60, 110);
        std::vector<int16_t> buf(2 * 2048);
        dry.processBlock(buf.data(), 2048);
        int peakL = 0, peakR = 0;
        for (size_t i = 0; i < 2048; ++i) {
            peakL = std::max(peakL, std::abs(static_cast<int>(buf[2 * i])));
            peakR = std::max(peakR, std::abs(static_cast<int>(buf[2 * i + 1])));
        }
        assert(peakL > 1000 && peakR == 0);
    }
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_6, 20);
    assert(!engine.isMonoOutput() && hudText == "STEREO");
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_6, 127);

    // R7 silences the metronome click; beats still count so the TAP LED and OLED keep flashing
    engine.setMetronomeEnabled(true);
    const uint32_t beatsBefore = engine.getMetroClickCount();
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_7, 90);
    assert(surface.getLastTargetCc() == Synth::CC_METRO_SILENT);
    assert(engine.isMetronomeSilent() && hudText == "SILENT");
    {
        Synth::SynthEngine quiet;
        quiet.init(44100);
        quiet.setMetronomeBpm(240.0f);
        quiet.controlChange(Synth::CC_METRO_SILENT, 127);
        quiet.setMetronomeEnabled(true);
        std::vector<int16_t> buf(2 * 512);
        int peak = 0;
        for (int block = 0; block < 100; ++block) { // ~1.2 s: several beats at 240 BPM
            quiet.processBlock(buf.data(), 512);
            for (int16_t v : buf) peak = std::max(peak, std::abs(static_cast<int>(v)));
        }
        assert(peak == 0);
        assert(quiet.getMetroClickCount() >= 4);
    }
    assert(engine.getMetroClickCount() >= beatsBefore);
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_7, 10);
    assert(!engine.isMetronomeSilent() && hudText == "SOUND");
    engine.setMetronomeEnabled(false);

    // R8 is master volume
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_8, 64);
    assert(surface.getLastTargetCc() == Synth::CC_VOLUME);

    // Reopening the page seeds R5 / R6 from the engine
    surface.setRhythmPageActive(false);
    surface.setRhythmPageActive(true);
    assert(surface.getRhythmValues()[4] * 13 / 128 == 6); // F
    assert(surface.getRhythmValues()[5] == 127);

    // 3. Plain BANK closes it and returns to Bank B with its knob values restored
    pluginPackets.clear();
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 0);
    assert(!surface.isRhythmPageActive() && surface.isBankBActive());
    seeds = knobSeeds();
    assert(seeds.size() == 8);
    const auto& juno = Synth::getPresets()[0];
    for (size_t k = 0; k < 8; ++k) assert(seeds[k] == juno.bankB.defaults[k]);
    surface.processMidiMessage(0xB0, Synth::CC_HW_KNOB_1, 50);
    assert(surface.getLastTargetCc() == juno.bankB.ccs[0]);

    // 4. SHIFT + BANK toggles it closed too; Bank Left/Right and preset changes also close it
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 127);
    assert(surface.isRhythmPageActive());
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 0);
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 127);
    assert(!surface.isRhythmPageActive() && surface.isBankBActive());
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 0);
    surface.processMidiMessage(0xB0, Synth::CC_HW_BANK_AB_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 0);
    assert(surface.isRhythmPageActive());
    surface.processMidiMessage(0xB0, Synth::CC_HW_CUSTOM_BANK_LEFT, 127);
    assert(!surface.isRhythmPageActive() && !surface.isBankBActive());
    surface.setRhythmPageActive(true);
    surface.selectPreset(1);
    assert(!surface.isRhythmPageActive());

    // 5. Tempo and time signature are global: no synth change resets them
    for (uint8_t i = 0; i < static_cast<uint8_t>(Synth::SynthModel::Count); ++i) {
        surface.selectPreset(i);
        assert(engine.getMetronomeBpm() == 240.0f);
        assert(engine.getTimeSigBeatsPerBar() == 16 && engine.getTimeSigBeatUnit() == 8);
        assert(engine.isMonoOutput());
    }
    surface.processMidiMessage(0xB0, Synth::CC_HW_PRESET_NEXT, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_NAV_ENCODER, 1);
    assert(engine.getMetronomeBpm() == 240.0f);
    assert(engine.getTimeSigBeatsPerBar() == 16 && engine.getTimeSigBeatUnit() == 8);

    std::cout << "  -> SHIFT+BANK Rhythm Page tests PASSED!" << std::endl;
}

void test_keybed_octave_transpose_tracking() {
    std::cout << "[Test] Running Keybed Octave/Transpose Tracking (SysEx 0x6A) test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    ControlSurface surface;
    surface.init(engine);

    // 1. Defaults: centre octave (4), transpose 0 -> no shift
    assert(surface.getKeybedOctave() == 4);
    assert(surface.getKeybedTranspose() == 0);
    assert(surface.getKeybedNoteOffset() == 0);

    // 2. Unsolicited notification: octave (idx 0x02) -> 5  => +12 semitones
    const uint8_t octUp[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x03, 0x00, 0x02, 0x05, 0xF7};
    assert(surface.processSysEx(octUp, sizeof(octUp)));
    assert(surface.getKeybedOctave() == 5);
    assert(surface.getKeybedNoteOffset() == 12);

    // 3. Octave down to 2 => -24 semitones
    const uint8_t octDown[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x03, 0x00, 0x02, 0x02, 0xF7};
    assert(surface.processSysEx(octDown, sizeof(octDown)));
    assert(surface.getKeybedNoteOffset() == -24);

    // 4. Transpose (idx 0x03, 12 = 0): 14 => +2 semitones, combined with octave 2
    const uint8_t trUp[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x03, 0x00, 0x03, 0x0E, 0xF7};
    assert(surface.processSysEx(trUp, sizeof(trUp)));
    assert(surface.getKeybedTranspose() == 2);
    assert(surface.getKeybedNoteOffset() == -22);

    // 4b. OCT+/- button press is announced as cmd 0x3B (not 0x6A): F0 47 <dev> 5D 3B 00 01 <oct> F7
    const uint8_t btnOct[] = {0xF0, 0x47, 0x00, 0x5D, 0x3B, 0x00, 0x01, 0x06, 0xF7};
    assert(surface.processSysEx(btnOct, sizeof(btnOct)));
    assert(surface.getKeybedOctave() == 6);
    assert(surface.getKeybedNoteOffset() == 24 + 2);
    const uint8_t btnBad[] = {0xF0, 0x47, 0x00, 0x5D, 0x3B, 0x00, 0x01, 0x09, 0xF7};
    assert(!surface.processSysEx(btnBad, sizeof(btnBad)));
    assert(surface.getKeybedOctave() == 6);
    assert(surface.processSysEx(octDown, sizeof(octDown)));  // restore octave 2 for the checks below
    assert(surface.getKeybedNoteOffset() == -22);

    // 5. Other parameter indices are ignored (idx 0x00 = keybed channel)
    const uint8_t chan[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x03, 0x00, 0x00, 0x09, 0xF7};
    surface.processSysEx(chan, sizeof(chan));
    assert(surface.getKeybedNoteOffset() == -22);

    // 6. Out-of-range octave value is rejected (valid 0..8)
    const uint8_t bad[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x03, 0x00, 0x02, 0x7F, 0xF7};
    surface.processSysEx(bad, sizeof(bad));
    assert(surface.getKeybedOctave() == 2);

    // 7. Query builder: F0 47 <dev> 5D 6A 00 02 <idxHi> <idxLo> F7
    auto q = DisplayDriver::makeParamQuerySysEx(0x02, 0x00);
    const std::vector<uint8_t> expected = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x02, 0x00, 0x02, 0xF7};
    assert(q == expected);

    // 8. requestKeybedState() sends octave + transpose queries over the Plugin Port
    std::vector<std::vector<uint8_t>> pluginPackets;
    surface.setPluginPortSender([&](const uint8_t* data, size_t len) {
        pluginPackets.emplace_back(data, data + len);
    });
    pluginPackets.clear(); // connecting the port emits its own sync packets
    assert(surface.requestKeybedState());
    assert(pluginPackets.size() == 2);
    assert(pluginPackets[0] == expected);
    assert(pluginPackets[1][8] == 0x03);

    std::cout << "  -> Keybed Octave/Transpose Tracking tests PASSED!" << std::endl;
}

void test_color_565_conversion() {
    std::cout << "[Test] Running 8-bit -> RGB565 color conversion..." << std::endl;
    // Monotonic across the old 31/32 boundary (the previous 0..63 scheme sent 31 as full red and 32 as half red)
    assert(Color(124, 0, 0).r5() == 15 && Color(128, 0, 0).r5() == 16);
    assert(Color(255, 255, 255).rgb565() == 0xFFFF);
    assert(Color::hex(0xFF6A1F).r5() == 31 && Color::hex(0xFF6A1F).g6() == 26 && Color::hex(0xFF6A1F).b5() == 3);
    assert(Color::fromNative(31, 63, 31) == Color::White());
    // 0x14 carries R5 G6 B5 exactly
    auto fmt = DisplayDriver::makeLineFormatSysEx(1, Align::Center, Color::hex(0xFF6A1F), Color::hex(0x120704));
    assert(fmt[4] == 0x14 && fmt[10] == 31 && fmt[11] == 26 && fmt[12] == 3);
    assert(fmt[13] == 2 && fmt[14] == 1 && fmt[15] == 0);
    std::cout << "  -> Color conversion PASSED!" << std::endl;
}

void test_blit_packet_format() {
    std::cout << "[Test] Running 0x71 pixel blit packet format..." << std::endl;
    const uint16_t px[2] = {0xF800, 0x07FF};
    auto sx = DisplayDriver::makeBlitSysEx(130, 73, 2, 1, px);
    // F0 47 00 5D 71 <len> | x=130 -> 01 02 | y=73 -> 00 49 | w=2 | h=1 | R5 G6 B5 x2 | F7
    const std::vector<uint8_t> want = {0xF0, 0x47, 0x00, 0x5D, 0x71, 0x00, 14, 0x01, 0x02, 0x00, 0x49, 0x00, 0x02,
                                       0x00, 0x01, 31, 0, 0, 0, 63, 31, 0xF7};
    assert(sx == want);
    // Payload length above 127 uses both length bytes
    std::vector<uint16_t> big(100, 0);
    auto sx2 = DisplayDriver::makeBlitSysEx(0, 0, 100, 1, big.data());
    const size_t payload = 8 + 300;
    assert(sx2[5] == (payload >> 7) && sx2[6] == (payload & 0x7F) && sx2.size() == payload + 8);
    std::cout << "  -> Blit packet format PASSED!" << std::endl;
}

void test_theme_lookup() {
    std::cout << "[Test] Running per-instrument theme lookup..." << std::endl;
    auto juno = DisplayDriver::themeForPresetName("JUNO-60");
    assert(juno.accent == Color::hex(0xFF6A1F));
    assert(juno.headerBg == juno.accent);
    assert(juno.bodyBg.luma() < 20 && juno.valueFg.luma() > juno.accent.luma());
    // Abbreviated factory names must still match
    assert(DisplayDriver::themeForPresetName("PT-10 FNTSY").accent == Color::hex(0x9C8CFF));
    assert(DisplayDriver::themeForPresetName("PT-10 VIOLN").accent == Color::hex(0xE89A5C));
    // "DX E-PIANO" is FM keys, not the Casio piano
    assert(std::string(DisplayDriver::themeForPresetName("DX E-PIANO").family) == "FM keys");
    assert(std::string(DisplayDriver::themeForPresetName("PT-10 PIANO").family) == "Lo-fi keys");
    assert(std::string(DisplayDriver::themeForPresetName("PPG WAVE").family) == "Wavetable");
    assert(std::string(DisplayDriver::themeForPresetName("CZ RESO").family) == "Phase dist");
    assert(std::string(DisplayDriver::themeForPresetName("KS KOTO").family) == "Plucked string");
    assert(std::string(DisplayDriver::themeForPresetName("B3 GOSPEL").family) == "Tonewheel organ");
    // Bank B inverts the banner instead of shifting the hue
    auto junoB = DisplayDriver::themeForPresetName("JUNO-60", true);
    assert(junoB.accent == juno.accent && junoB.headerFg == juno.accent && junoB.headerBg.luma() < juno.accent.luma());
    assert(std::string(DisplayDriver::themeForPresetName("My Patch").family) == "User patch");
    std::cout << "  -> Theme lookup PASSED!" << std::endl;
}

void test_host_value_draws_only_changed_pixels() {
    std::cout << "[Test] Running host-drawn value diff blits..." << std::endl;
    DisplayDriver drv;
    drv.setScreenTakeoverEnabled(false);
    std::vector<std::vector<uint8_t>> pkts;
    drv.setPluginPortSender([&](const uint8_t* d, size_t n) { pkts.emplace_back(d, d + n); });
    auto theme = DisplayDriver::themeForPresetName("JUNO-60");

    // First call claims the screen and writes the lines; pixels wait until the MK4 has applied both
    drv.showKnobHud("JUNO-60", "Cutoff", "42%", 53, theme);
    size_t lineWrites = 0, blits = 0;
    for (const auto& p : pkts) {
        lineWrites += (p[4] == 0x10 || p[4] == 0x14);
        blits += (p[4] == 0x71);
    }
    assert(pkts[0][4] == 0x1C && lineWrites == 6 && blits == 0 && drv.hasDeferredHostDraw());
    assert(drv.getHostValueText().empty());

    // Follow-up call: strip, value and bar, no line writes
    pkts.clear();
    drv.showKnobHud("JUNO-60", "Cutoff", "42%", 53, theme);
    assert(!pkts.empty() && !drv.hasDeferredHostDraw() && drv.getHostValueText() == "42%");
    for (const auto& p : pkts) {
        assert(p[4] == 0x71);
        assert(p.size() <= 2000); // chunked for the ESP32 USB host queue
    }

    // Same text: nothing; one digit changes: a blit narrower than the whole "42%"
    pkts.clear();
    drv.showKnobHud("JUNO-60", "Cutoff", "42%", 53, theme);
    assert(pkts.empty());
    drv.showKnobHud("JUNO-60", "Cutoff", "43%", 53, theme);
    assert(pkts.size() == 1 && pkts[0][4] == 0x71);
    const int w = (pkts[0][11] << 7) | pkts[0][12];
    assert(w > 0 && w < 12); // one digit cell, the '4' and '%' are untouched

    // Bar only: value unchanged, knob moved -> a small bar delta
    pkts.clear();
    drv.showKnobHud("JUNO-60", "Cutoff", "43%", 60, theme);
    assert(pkts.size() == 1 && pkts[0][4] == 0x71);
    assert(((pkts[0][9] << 7) | pkts[0][10]) == HostLayer::BAR_Y);

    // Home screen afterwards: line 2 becomes the family tag again and the bar is cleared
    pkts.clear();
    drv.showHomeScreen("JUNO-60", "1/32 | BANK A", theme);
    assert(drv.getLastLines()[2].text == "Analog poly");
    assert(drv.getHostValueText().empty());

    // Back to the knob screen: line 2 is rewritten, so the value is held back one call (the MK4 repaints the line
    // later and would wipe it), while the bar in rows 72..79 is drawn at once
    pkts.clear();
    drv.showKnobHud("JUNO-60", "Cutoff", "50%", 64, theme);
    assert(drv.hasDeferredHostDraw() && drv.getHostValueText().empty());
    int valueRowBlits = 0;
    for (const auto& p : pkts) {
        if (p[4] != 0x71) continue;
        const int y = (p[9] << 7) | p[10];
        valueRowBlits += (y >= HostLayer::VALUE_Y && y < HostLayer::VALUE_Y + HostLayer::VALUE_H);
    }
    assert(valueRowBlits == 0);
    drv.showKnobHud("JUNO-60", "Cutoff", "50%", 64, theme);
    assert(!drv.hasDeferredHostDraw() && drv.getHostValueText() == "50%");
    std::cout << "  -> Host value diff blits PASSED!" << std::endl;
}

// Preset / bank navigation CCs are handled here (not in SynthEngine), so the knob state always follows the preset
void test_navigation_ccs() {
    std::cout << "[Test] Running Navigation CC (SHIFT +/-, Bank Select, cursor keys, foot switch) test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);
    ControlSurface surface;
    surface.init(engine);
    surface.setPluginPortSender([](const uint8_t*, size_t) {});
    surface.selectPreset(0);

    // 1. '+' / '-' step the preset; with SHIFT they step the OLED animation and leave the preset alone
    int animSteps = 0;
    surface.setOnAnimationStep([&](int step) { animSteps += step; });
    surface.processMidiMessage(0xB0, Synth::CC_HW_PRESET_NEXT, 127);
    assert(surface.getCurrentPreset() == 1 && surface.getLastUiEvent() == UiEvent::PRESET_NEXT);
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_PRESET_NEXT, 127);
    assert(surface.getLastUiEvent() == UiEvent::ANIM_NEXT);
    surface.processMidiMessage(0xB0, Synth::CC_HW_PRESET_PREV, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_PRESET_PREV, 127);
    assert(surface.getLastUiEvent() == UiEvent::ANIM_PREV);
    assert(animSteps == -1 && surface.getCurrentPreset() == 1);

    // 2. SHIFT + encoder push does not toggle the metronome (SHIFT + TAP TEMPO does)
    const bool metroBefore = engine.isMetronomeEnabled();
    surface.processMidiMessage(0xB0, Synth::CC_HW_PUSH, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_PUSH, 0);
    assert(engine.isMetronomeEnabled() == metroBefore);
    surface.processMidiMessage(0xB0, Synth::CC_HW_SHIFT_BUTTON, 0);

    // 3. Bank Select picks a slot and resets the knobs to that preset's defaults
    surface.processMidiMessage(0xB0, Synth::CC_BANK_SELECT_MSB, 6);
    assert(surface.getCurrentPreset() == 6);
    assert(engine.getCurrentPreset().model == Synth::SynthModel::CasioPT10_Piano);
    assert(surface.getBankAValues()[0] == engine.getCurrentPreset().bankA.defaults[0]);

    // 4. Cursor / transport keys step through the slots (CC 110 / 114 back, CC 111 / 115 forward)
    surface.processMidiMessage(0xB0, Synth::CC_HW_CURSOR_RIGHT, 127);
    assert(engine.getCurrentPreset().model == Synth::SynthModel::CasioPT10_Fantasy);
    surface.processMidiMessage(0xB0, Synth::CC_HW_TRANSPORT_NEXT, 127);
    assert(engine.getCurrentPreset().model == Synth::SynthModel::CasioPT10_Violin);
    surface.processMidiMessage(0xB0, Synth::CC_HW_CURSOR_LEFT, 127);
    assert(engine.getCurrentPreset().model == Synth::SynthModel::CasioPT10_Fantasy);
    surface.processMidiMessage(0xB0, Synth::CC_HW_TRANSPORT_PREV, 127);
    assert(engine.getCurrentPreset().model == Synth::SynthModel::CasioPT10_Piano);
    assert(surface.getCurrentPreset() == 6);

    // 5. The foot switch toggles the bank on each press, not on release
    surface.processMidiMessage(0xB0, Synth::CC_HW_FOOTSWITCH_BANK, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_FOOTSWITCH_BANK, 0);
    assert(surface.isBankBActive());
    surface.processMidiMessage(0xB0, Synth::CC_HW_FOOTSWITCH_BANK, 127);
    surface.processMidiMessage(0xB0, Synth::CC_HW_FOOTSWITCH_BANK, 0);
    assert(!surface.isBankBActive());

    // 6. The engine itself ignores navigation CCs, so it cannot change preset behind the surface's back
    engine.controlChange(Synth::CC_HW_CURSOR_RIGHT, 127);
    assert(engine.getCurrentPreset().model == Synth::SynthModel::CasioPT10_Piano);

    std::cout << "  -> Navigation CC tests PASSED!" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "  Running MpkMiniMk4::ControlSurface Unit Tests   " << std::endl;
    std::cout << "==================================================" << std::endl;

    test_plugin_port_required();
    test_preset_load_pre_seeds_8_knobs();
    test_physical_knob_turn_and_echo_suppression();
    test_bank_switch_preserves_tweaked_values();
    test_external_parameter_change_pre_seeds_hardware();
    test_compound_gestures();
    test_screen_owner_heartbeat_keepalive();
    test_shift_button_cc17_and_hud_auto_dismiss();
    test_physical_tap_tempo_cc82();
    test_shift_bank_opens_rhythm_page();
    test_knob_hud_without_takeover();
    test_preset_name_hud_and_native_menu_yield();
    test_keybed_octave_transpose_tracking();
    test_color_565_conversion();
    test_blit_packet_format();
    test_theme_lookup();
    test_host_value_draws_only_changed_pixels();
    test_navigation_ccs();

    std::cout << "\n>>> ALL CONTROL SURFACE TESTS PASSED (18/18)! <<<" << std::endl;
    return 0;
}
