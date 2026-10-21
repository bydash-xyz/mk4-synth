#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>

#define MOCK_HAL 1
#include "Arduino.h"
#include "Preferences.h"
#include "driver/i2s_std.h"

#include "SynthEngine.h"
#include "Presets.h"
#include "SysExProtocol.h"
#include "knob_page.h"
#include "oled_ui.h"
#include "animations/KeyboardLayout.h"
#include "animations/KeyboardAnimation.h"
#include "animations/RoadRacerAnimation.h"
#include "animations/SynthwaveAnimation.h"
#include "animations/LissajousAnimation.h"
#include "ble_midi_server.h"
#include "usb_host_midi.h"
#include "system_status.h"

using namespace HardwareDriver;

void test_mpk_knob_pre_seeding() {
    std::cout << "[Test] Running MpkMiniMk4 SysEx 0x12 Knob Pre-Seeding test..." << std::endl;
    // 1. Verify 10-byte SysEx 0x12 packet structure: F0 47 <dev> 5D 12 00 02 <k> <val> F7
    auto pktK0 = MpkMiniMk4::DisplayDriver::makeKnobValueSysEx(0, 95);
    assert(pktK0.size() == 10);
    assert(pktK0[0] == 0xF0);
    assert(pktK0[1] == 0x47); // Akai Manufacturer ID
    assert(pktK0[2] == 0x00); // Default device ID
    assert(pktK0[3] == 0x5D); // Model ID: MPK Mini IV
    assert(pktK0[4] == 0x12); // Command 0x12: Set Hardware Knob Value
    assert(pktK0[5] == 0x00); // Length MSB
    assert(pktK0[6] == 0x02); // Length LSB = 2
    assert(pktK0[7] == 0x00); // Knob 0 (K1)
    assert(pktK0[8] == 95);   // Value 95
    assert(pktK0[9] == 0xF7);

    // 2. Test bounds clamping (knobIndex clamped to 7, value clamped to 127)
    auto pktClamped = MpkMiniMk4::DisplayDriver::makeKnobValueSysEx(15, 200, 0x02);
    assert(pktClamped.size() == 10);
    assert(pktClamped[2] == 0x02); // devId
    assert(pktClamped[7] == 0x07); // Clamped knob index 7
    assert(pktClamped[8] == 127);  // Clamped value 127

    std::cout << "  -> MpkMiniMk4 SysEx 0x12 tests PASSED!" << std::endl;
}

void test_mpk_bank_select() {
    std::cout << "[Test] Running MpkMiniMk4 SysEx 0x3A Bank Select test..." << std::endl;
    // 1. Bank A (White LED): F0 47 <dev> 5D 3A 00 01 01 F7
    auto pktBankA = MpkMiniMk4::DisplayDriver::makeBankSelectSysEx(false);
    assert(pktBankA.size() == 9);
    assert(pktBankA[0] == 0xF0);
    assert(pktBankA[1] == 0x47);
    assert(pktBankA[2] == 0x00);
    assert(pktBankA[3] == 0x5D);
    assert(pktBankA[4] == 0x3A); // Command 0x3A (Bank Select & LED)
    assert(pktBankA[5] == 0x00);
    assert(pktBankA[6] == 0x01); // Length 1
    assert(pktBankA[7] == 0x01); // Bank A (White LED)
    assert(pktBankA[8] == 0xF7);

    // 2. Bank B (Red LED): F0 47 <dev> 5D 3A 00 01 00 F7
    auto pktBankB = MpkMiniMk4::DisplayDriver::makeBankSelectSysEx(true, 0x03);
    assert(pktBankB.size() == 9);
    assert(pktBankB[0] == 0xF0);
    assert(pktBankB[1] == 0x47);
    assert(pktBankB[2] == 0x03); // Custom devId
    assert(pktBankB[3] == 0x5D);
    assert(pktBankB[4] == 0x3A);
    assert(pktBankB[5] == 0x00);
    assert(pktBankB[6] == 0x01);
    assert(pktBankB[7] == 0x00); // Bank B (Red LED)
    assert(pktBankB[8] == 0xF7);

    // 3. Test selectHardwareBank method with injected transport
    MpkMiniMk4::DisplayDriver driver;
    std::vector<std::vector<uint8_t>> sentPackets;
    driver.setPluginPortSender([&](const uint8_t* data, size_t len) {
        sentPackets.emplace_back(data, data + len);
    });

    driver.selectHardwareBank(false);
    assert(sentPackets.size() == 1);
    assert(sentPackets[0] == pktBankA);

    sentPackets.clear();
    driver.selectHardwareBank(true);
    assert(sentPackets.size() == 1);
    assert(sentPackets[0][7] == 0x00);

    std::cout << "  -> MpkMiniMk4 SysEx 0x3A Bank Select tests PASSED!" << std::endl;
}

void test_ble_midi_and_nvs_persistence() {
    std::cout << "[Test] Running BleMidiServer & NVS Persistence test..." << std::endl;
    Preferences::clearAllStorage();

    Synth::SynthEngine engine;
    engine.init(44100);

    BleMidiServer ble;
    ble.init(engine);

    // 1. Create a 48-byte synth pad patch
    Synth::RawPatchData testPad;
    std::memset(&testPad, 0, sizeof(testPad));
    std::strncpy(testPad.name, "DREAM PAD", 11);
    testPad.model = static_cast<uint8_t>(Synth::SynthModel::User);
    testPad.waveform = static_cast<uint8_t>(Synth::Waveform::Triangle);
    testPad.voiceMode = static_cast<uint8_t>(Synth::VoiceMode::Polyphonic);
    testPad.flags = 0x01; // Chorus enabled
    testPad.cutoffHz = 850;
    testPad.resonanceQ = 2000;
    testPad.subOscLevel = 16000;
    testPad.ampAttackMs = 1000;  // 1s pad attack
    testPad.ampReleaseMs = 2000; // 2s lush pad release

    // Encode to 62-byte SysEx
    uint8_t dumpBuffer[Synth::FULL_DUMP_PACKET_SIZE];
    size_t encodedLen = Synth::SysExProtocol::encodePatchDump(
        Synth::USER_PRESET_SLOT,
        testPad,
        dumpBuffer,
        sizeof(dumpBuffer)
    );
    assert(encodedLen == Synth::FULL_DUMP_PACKET_SIZE);

    // Simulate wireless SysEx patch drop over BLE
    bool handled = ble.handleSysExReceived(dumpBuffer, encodedLen);
    assert(handled == true);

    // Verify engine has adopted new parameters immediately
    const auto& userPreset = engine.getUserPreset();
    assert(std::strncmp(userPreset.name, "DREAM PAD", 9) == 0);
    assert(userPreset.waveform == Synth::Waveform::Triangle);

    // 2. Test simulated device restart: new SynthEngine & BleMidiServer instances
    Synth::SynthEngine rebootedEngine;
    rebootedEngine.init(44100);
    BleMidiServer rebootedBle;
    rebootedBle.init(rebootedEngine);

    // Verify user preset was restored from NVS automatically on boot
    const auto& restoredPreset = rebootedEngine.getUserPreset();
    assert(std::strncmp(restoredPreset.name, "DREAM PAD", 9) == 0);
    assert(restoredPreset.waveform == Synth::Waveform::Triangle);

    // The v1 dump was stored as a v2 body under q_24, and its ACK queued for the BLE central
    assert(Preferences::s_storage.count("synth:q_37") == 1);
    assert(Preferences::s_storage.count("synth:p_37") == 0);
    auto ack = Synth::SysExProtocol::decodeSysEx(ble.getLastSentResponse().data(), ble.getLastSentResponse().size());
    assert(ack.isValid && ack.type == Synth::SysExCommand::Handshake);
    assert(ack.refCommand == Synth::SysExCommand::PatchDump && ack.handshakeStatus == Synth::HandshakeStatus::AckSuccess);

    // 3. PatchDumpV2 over BLE: FM patch with its tremolo rate, ACKed as 0x05, persisted across reboot
    Synth::Preset rhodes = Synth::getPresets()[static_cast<size_t>(Synth::SynthModel::FM_WarmRhodes)];
    rhodes.name = "MY RHODES";
    uint8_t v2Buf[Synth::MAX_SYSEX_MESSAGE_SIZE];
    size_t v2Len = Synth::SysExProtocol::encodePatchDumpV2(41, rhodes, v2Buf, sizeof(v2Buf));
    assert(rebootedBle.handleSysExReceived(v2Buf, v2Len));
    ack = Synth::SysExProtocol::decodeSysEx(rebootedBle.getLastSentResponse().data(), rebootedBle.getLastSentResponse().size());
    assert(ack.refCommand == Synth::SysExCommand::PatchDumpV2 && ack.handshakeStatus == Synth::HandshakeStatus::AckSuccess);

    // A reply to SysEx handled outside BLE (e.g. USB) is not sent on the next BLE message, and a fast
    // parameter update (which has no reply) sends nothing
    const uint8_t usbRequest[] = {0xF0, 0x7D, 0x01, 0x04, 0x01, 0xF7};
    rebootedEngine.handleSysEx(usbRequest, sizeof(usbRequest));
    uint8_t fp[10];
    Synth::SysExProtocol::encodeFastParamUpdate(Synth::ParamId::Cutoff, 8000, 0, fp, sizeof(fp));
    rebootedBle.handleSysExReceived(fp, sizeof(fp));
    ack = Synth::SysExProtocol::decodeSysEx(rebootedBle.getLastSentResponse().data(), rebootedBle.getLastSentResponse().size());
    assert(ack.type == Synth::SysExCommand::Handshake && ack.refCommand == Synth::SysExCommand::PatchDumpV2);

    // 4. Legacy v1 blob under p_25 (written by older firmware) is loaded when q_25 is absent
    Synth::Preset old = Synth::getPresets()[static_cast<size_t>(Synth::SynthModel::OBXa)];
    old.name = "OLD OBXA";
    Synth::RawPatchData oldRaw;
    Synth::SysExProtocol::presetToRawPatch(old, oldRaw);
    const uint8_t* oldBytes = reinterpret_cast<const uint8_t*>(&oldRaw);
    Preferences::s_storage["synth:p_38"] = std::vector<uint8_t>(oldBytes, oldBytes + sizeof(oldRaw));
    // An oversized q_ blob (corrupt or from a future format) is ignored rather than truncated
    Preferences::s_storage["synth:q_42"] = std::vector<uint8_t>(Synth::MAX_PATCH_BODY_SIZE + 1, 0x01);

    Synth::SynthEngine thirdEngine;
    thirdEngine.init(44100);
    BleMidiServer thirdBle;
    thirdBle.init(thirdEngine);
    assert(std::strcmp(thirdEngine.getUserPreset(38).name, "OLD OBXA") == 0);
    assert(thirdEngine.getUserPreset(38).voicing == Synth::SynthModel::OBXa);
    assert(std::strcmp(thirdEngine.getUserPreset(41).name, "MY RHODES") == 0);
    assert(std::abs(thirdEngine.getUserPreset(41).tremoloRateHz - 4.2f) < 0.01f);
    assert(std::strcmp(thirdEngine.getUserPreset(42).name, "USER 06") == 0);
    assert(Preferences::s_storage.count("synth:q_38") == 0); // not rewritten at boot

    // 5. Upgrade -> downgrade -> upgrade: slot 25 is saved as v2, then older firmware saves a new p_25.
    //    On the next boot of v2 firmware the newer p_25 wins over the stale q_25.
    assert(thirdEngine.saveUserPresetToStorage(38));
    assert(Preferences::s_storage.count("synth:q_38") == 1);
    Synth::Preset newer = Synth::getPresets()[static_cast<size_t>(Synth::SynthModel::CS80)];
    newer.name = "DOWNGRADE";
    Synth::RawPatchData newerRaw;
    Synth::SysExProtocol::presetToRawPatch(newer, newerRaw);
    const uint8_t* newerBytes = reinterpret_cast<const uint8_t*>(&newerRaw);
    Preferences::s_storage["synth:p_38"] = std::vector<uint8_t>(newerBytes, newerBytes + sizeof(newerRaw));

    Synth::SynthEngine fourthEngine;
    fourthEngine.init(44100);
    BleMidiServer fourthBle;
    fourthBle.init(fourthEngine);
    assert(std::strcmp(fourthEngine.getUserPreset(38).name, "DOWNGRADE") == 0);
    assert(std::strcmp(fourthEngine.getUserPreset(41).name, "MY RHODES") == 0); // untouched slots keep v2

    std::cout << "  -> BleMidiServer & NVS Persistence tests PASSED!" << std::endl;
}

void test_usb_host_midi() {
    std::cout << "[Test] Running UsbHostMidi test..." << std::endl;
    ScopedFrozenMillis frozenClock; // BANK A/B press + release must not look like a 400 ms hold
    Synth::SynthEngine engine;
    engine.init(44100);

    UsbHostMidi usb;
    usb.init(engine);

    // MPK octave notification (0x6A, idx 0x02 -> octave 3) shifts keybed notes by -12
    assert(usb.getKeybedNoteOffset() == 0);
    const uint8_t octNotify[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x03, 0x00, 0x02, 0x03, 0xF7};
    usb.processSysEx(octNotify, sizeof(octNotify));
    assert(usb.getKeybedNoteOffset() == -12);
    const uint8_t octReset[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x03, 0x00, 0x02, 0x04, 0xF7};
    usb.processSysEx(octReset, sizeof(octReset));
    assert(usb.getKeybedNoteOffset() == 0);

    bool presetChangedTriggered = false;
    uint8_t reportedPreset = 0;
    std::string reportedName = "";

    usb.setOnPresetChanged([&](uint8_t idx, const char* name) {
        presetChangedTriggered = true;
        reportedPreset = idx;
        reportedName = name ? name : "";
    });

    std::vector<uint8_t> lastDispatchedSysEx;
    usb.setOnSysExOut([&](const std::vector<uint8_t>& packet) {
        lastDispatchedSysEx = packet;
    });

    // 1. Test Note On / Note Off
    usb.processMidiMessage(0x90, 60, 100); // NoteOn C4, vel 100
    assert(engine.getActiveVoiceCount() == 1);
    usb.processMidiMessage(0x80, 60, 0);   // NoteOff C4
    // Voice releases into release phase
    assert(engine.getActiveVoiceCount() == 1);

    // 2. Test Master Push-Encoder Rotation (Preset Cycling)
    // CC 14, value 1 -> increment preset
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MASTER_ENCODER, 1);
    assert(presetChangedTriggered == true);
    assert(reportedPreset == 1);
    assert(reportedName == "MINIMOOG");

    // 3. Test Bank Right / Left Buttons (Bank B / Bank A Knob Paging without switching preset)
    bool pageChangedTriggered = false;
    usb.setOnPageChanged([&](KnobPage page) {
        pageChangedTriggered = true;
        assert(page == KnobPage::BANK_B || page == KnobPage::BANK_A);
    });

    usb.processMidiMessage(0xB0, UsbHostMidi::CC_BANK_RIGHT, 127);
    assert(pageChangedTriggered == true);
    assert(usb.isBankBLatched() == true);
    assert(usb.isBankBActive() == true);
    assert(usb.getCurrentPreset() == 1); // Preset stays 1 (MINIMOOG); Bank Right only switches knob bank

    usb.processMidiMessage(0xB0, UsbHostMidi::CC_BANK_LEFT, 127);
    assert(usb.isBankBLatched() == false);
    assert(usb.isBankBActive() == false);

    // Advance to preset 2 (PROPHET-5) via '+' button before testing Shift +4 jump
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_PLUS, 127);
    assert(usb.getCurrentPreset() == 2);

    // 4. Test Shift Modifier Jump
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 127); // Hold shift
    assert(usb.isShiftHeld() == true);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MASTER_ENCODER, 1); // Advance by 4
    assert(usb.getCurrentPreset() == 6); // 2 + 4 = 6

    // 5. Test SHIFT + Keybed NoteOn -> Instant Root Key Selection (without triggering a voice)
    engine.panic();
    usb.processMidiMessage(0x90, 66, 100); // Press F#4 (66 % 12 = 6 = F#) while SHIFT is held
    assert(engine.getRootKey() == 6);
    assert(std::strcmp(engine.getRootKeyName(), "F#") == 0);
    assert(engine.getActiveVoiceCount() == 0); // No synth note triggered while choosing Root Key!

    // 6. Test SHIFT + TAP TEMPO (Metronome key on MPK Mini MK4) -> Toggles Metronome ON
    assert(engine.isMetronomeEnabled() == false);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_TAP_TEMPO_BTN, 127); // Press TAP TEMPO while SHIFT is held
    assert(engine.isMetronomeEnabled() == true);

    // SHIFT + encoder push only refreshes the MPK screen; TAP TEMPO is the sole metronome toggle
    usb.processMidiMessage(0xB0, Synth::CC_HW_PUSH, 127);
    usb.processMidiMessage(0xB0, Synth::CC_HW_PUSH, 0);
    assert(engine.isMetronomeEnabled() == true);

    // Release shift and test Page 3 (Rhythm & Key) cycling
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 0);
    KnobPage lastReportedPage = KnobPage::BANK_A;
    usb.setOnPageChanged([&](KnobPage page) {
        lastReportedPage = page;
    });
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_BANK_RIGHT, 127);
    assert(usb.currentKnobPage() == KnobPage::BANK_B);
    assert(lastReportedPage == KnobPage::BANK_B);
    assert(usb.isBankBActive() == true);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_BANK_LEFT, 127);
    assert(usb.currentKnobPage() == KnobPage::BANK_A);
    assert(lastReportedPage == KnobPage::BANK_A);
    assert(usb.isBankBActive() == false);

    // 7. Test MPK Mini MK4 Hardware Preset Navigation: CC 81 (+), CC 80 (-), and CC 72 (Encoder)
    usb.selectPreset(0); // Start at Slot #1 (JUNO-60)
    assert(usb.getCurrentPreset() == 0);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_PLUS, 127); // '+' button -> Slot #2 (MINIMOOG)
    assert(usb.getCurrentPreset() == 1);
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::PRESET_NEXT);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_MINUS, 127); // '-' button -> Slot #1 (JUNO-60)
    assert(usb.getCurrentPreset() == 0);
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::PRESET_PREV);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_ENCODER, 1); // Encoder CW -> Slot #2 (MINIMOOG)
    assert(usb.getCurrentPreset() == 1);
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::PRESET_ENCODER);

    // 7a. SHIFT + '-' / '+' steps the OLED animation and leaves the preset alone
    int animStepSum = 0;
    usb.setOnAnimationStep([&](int step) { animStepSum += step; });
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_PLUS, 127);
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::ANIM_NEXT);
    assert(animStepSum == 1);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_PLUS, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_MINUS, 127);
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::ANIM_PREV);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_MINUS, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_MINUS, 127);
    assert(animStepSum == -1);
    assert(usb.getCurrentPreset() == 1);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 0);

    // 7b. CC 74 is the DAW transport button (NOT SHIFT): it must not engage SHIFT, change preset or hit the synth
    uint8_t presetBeforeTransport = usb.getCurrentPreset();
    usb.processMidiMessage(0xB0, Synth::CC_HW_DAW_TRANSPORT_BTN, 127);
    assert(usb.getCurrentPreset() == presetBeforeTransport);
    assert(usb.isShiftHeld() == false);
    usb.processMidiMessage(0xB0, Synth::CC_HW_DAW_TRANSPORT_BTN, 0);
    assert(usb.isShiftHeld() == false);

    // 8. Test Strict SHIFT Gate for Physical Knobs K1..K8 (CC 24..31: Top Row 24..27, Bottom Row 28..31)
    assert(usb.isShiftHeld() == false);
    usb.processMidiMessage(0xB0, 24, 95); // Turn Top-Left Knob 1 (CC 24) without Shift -> Bank A K1 (Cutoff CC 74)
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::KNOB_TURNED);
    assert(usb.getLastKnobIndex() == 0);
    assert(usb.getLastTargetCc() == 74); // Bank A K1 = Cutoff (74)

    usb.processMidiMessage(0xB0, 28, 88); // Turn Bottom-Left Knob 5 (CC 28) without Shift -> Bank A K5 (Attack CC 73)
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::KNOB_TURNED);
    assert(usb.getLastKnobIndex() == 4);
    assert(usb.getLastTargetCc() == 73); // Bank A K5 = Attack (73)
    assert(usb.isShiftHeld() == false);

    // SHIFT + knob = firmware fine adjust (delta/4 on the device). It must NOT switch bank:
    // the same knob keeps targeting Bank A K1 (Cutoff CC 74), and the bank stays A.
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 127); // Hold Shift
    assert(usb.isShiftHeld() == true);
    assert(usb.isBankBActive() == false);
    usb.processMidiMessage(0xB0, 24, 96);
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::KNOB_TURNED);
    assert(usb.getLastKnobIndex() == 0);
    assert(usb.getLastTargetCc() == 74); // Still Bank A K1 = Cutoff
    assert(usb.getLastKnobValue() == 96);
    usb.processMidiMessage(0xB0, 31, 105);
    assert(usb.getLastKnobIndex() == 7);
    assert(usb.getLastTargetCc() != 7);  // Bank A K8, not Bank B K16 (Master CC 7)
    assert(usb.isBankBActive() == false);

    // Bank B (latched via BANK A/B) + SHIFT still uses Bank B. SHIFT+BANK opens the Rhythm page, so latch B first.
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 0);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 0);
    assert(usb.isBankBActive() == true);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 127);
    usb.processMidiMessage(0xB0, 24, 110);
    assert(usb.getLastTargetCc() == 76); // Bank B K9 = FiltEnv
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 0);
    assert(usb.isBankBActive() == true); // Releasing SHIFT doesn't flip bank
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 0);
    assert(usb.isBankBActive() == false);

    // 9. Test Hardware BANK A/B Toggle Button (CC 50 / Synth::CC_HW_BANK_AB_BUTTON)
    assert(usb.isBankBActive() == false);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 127); // 1st Press BANK A/B -> Bank B latched
    assert(usb.isBankBActive() == true);
    assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::PAGE_CHANGED);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 0);   // Release BANK A/B button -> Bank B STAYS latched!
    assert(usb.isBankBActive() == true);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 127); // 2nd Press BANK A/B -> Toggles back to Bank A
    assert(usb.isBankBActive() == false);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 0);   // Release BANK A/B button -> Bank A stays active
    assert(usb.isShiftHeld() == false);

    // 9b. SHIFT + BANK opens the Rhythm page (page 3: tempo, time signature, key); a plain BANK press closes it
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 0);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_SHIFT_BUTTON, 0);
    assert(usb.isRhythmPageActive());
    assert(usb.currentKnobPage() == KnobPage::RHYTHM);
    assert(lastReportedPage == KnobPage::RHYTHM);
    usb.processMidiMessage(0xB0, Synth::CC_HW_KNOB_2, 127); // K2 = Tempo
    assert(usb.getLastTargetCc() == Synth::CC_TEMPO_BPM);
    assert(engine.getMetronomeBpm() == 240.0f);
    usb.processMidiMessage(0xB0, Synth::CC_HW_KNOB_3, 16); // K3 = Time Signature, 3/4 slot
    assert(engine.getTimeSigBeatsPerBar() == 3 && engine.getTimeSigBeatUnit() == 4);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_BANK_AB, 0);
    assert(!usb.isRhythmPageActive());
    assert(usb.isBankBActive() == false);
    assert(usb.currentKnobPage() == KnobPage::BANK_A);
    assert(lastReportedPage == KnobPage::BANK_A);
    // Changing synth (+ / - buttons, encoder, direct select) keeps tempo and time signature
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_PLUS, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_MINUS, 127);
    usb.processMidiMessage(0xB0, UsbHostMidi::CC_MASTER_ENCODER, 1);
    for (uint8_t i = 0; i < static_cast<uint8_t>(Synth::SynthModel::Count); ++i) {
        usb.selectPreset(i);
        assert(engine.getMetronomeBpm() == 240.0f);
        assert(engine.getTimeSigBeatsPerBar() == 3 && engine.getTimeSigBeatUnit() == 4);
    }
    usb.selectPreset(0);
    engine.setTimeSignature(4, 4);
    engine.setMetronomeBpm(120.0f);

    // 10. Test K3 (Synth::CC_HW_KNOB_3 = CC 26) DSP Parameter Mutation Across All Synth Engines
    // 10a. Juno-60 (Preset 0): K3 = SubOsc (CC 77); drive has its own CC and must not move
    usb.selectPreset(0);
    usb.processMidiMessage(0xB0, Synth::CC_HW_KNOB_3, 0);
    assert(std::abs(engine.getSubOscLevel() - 0.0f) < 0.01f);
    usb.processMidiMessage(0xB0, Synth::CC_HW_KNOB_3, 127);
    assert(usb.getLastKnobIndex() == 2);
    assert(usb.getLastTargetCc() == Synth::CC_SUB_OSC_LEVEL);
    assert(engine.getSubOscLevel() > 0.98f);
    assert(std::abs(engine.getDrive() - 1.0f) < 0.01f);

    // 10b. Casio PT-10 (Preset 6): K3 = VibRate (CC 89); vibrato depth is a separate knob
    usb.selectPreset(6);
    const float casioDepthBefore = engine.getVibratoDepth();
    usb.processMidiMessage(0xB0, Synth::CC_HW_KNOB_3, 100);
    assert(usb.getLastKnobIndex() == 2);
    assert(usb.getLastTargetCc() == Synth::CC_LFO_RATE);
    assert(engine.getVibratoRateHz() > 8.0f);
    assert(std::abs(engine.getVibratoDepth() - casioDepthBefore) < 0.001f);

    // 10c. Yamaha DX7 E-Piano (Preset 10, FM Engine): K3 = Feedback (CC 77)
    usb.selectPreset(10);
    usb.processMidiMessage(0xB0, Synth::CC_HW_KNOB_3, 127);
    assert(usb.getLastKnobIndex() == 2);
    assert(usb.getLastTargetCc() == Synth::CC_SUB_OSC_LEVEL);
    assert(engine.getModFeedback() > 1.4f);

    // 11. Verify Every Preset Step (+ / - / Encoder) Immediately Dispatches Display SysEx Without Requiring a Knob Turn
    for (uint8_t step = 0; step < 8; ++step) {
        lastDispatchedSysEx.clear();
        usb.processMidiMessage(0xB0, UsbHostMidi::CC_MPK4_PLUS, 127);
        assert(!lastDispatchedSysEx.empty());
        assert(usb.getLastUiEvent() == UsbHostMidi::MidiUiEvent::PRESET_NEXT);
    }

    std::cout << "  -> UsbHostMidi tests PASSED!" << std::endl;
}

void test_oled_ui() {
    std::cout << "[Test] Running OledDisplayUI & AnimationRegistry test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);

    AnimationRegistry registry;
    KeyboardAnimation keyboard;
    RoadRacerAnimation racer;
    SynthwaveAnimation synthwave;
    keyboard.init();
    racer.init();
    synthwave.init();

    assert(registry.registerAnimation(&keyboard));
    assert(registry.registerAnimation(&racer));
    assert(registry.registerAnimation(&synthwave));
    assert(registry.count() == 3);

    OledDisplayUI oled;
    bool ok = oled.init();
    assert(ok == true);
    assert(oled.getDisplay() != nullptr);
    oled.setAnimationRegistry(&registry);

    oled.setPreset(0, "JUNO-60");
    assert(oled.getActivePreset() == 0);
    assert(std::strcmp(oled.getPresetName(), "JUNO-60") == 0);

    // Trigger a note, notify registry, and render across all 3 animations
    engine.noteOn(60, 100);
    registry.onNoteOn(60, 100);
    oled.render(engine, 0.75f);
    assert(oled.getLastRenderedVoices() == 1);
    assert(std::abs(oled.getLastRenderedVol() - 0.75f) < 0.01f);
    assert(oled.getDisplay()->getDrawCallCount() > 0);

    oled.nextAnimation();
    assert(std::strcmp(registry.getActiveName(), "Road Racer") == 0);
    registry.onBeat(0, true);
    oled.render(engine, 0.75f);
    assert(oled.getDisplay()->getDrawCallCount() > 0);

    oled.nextAnimation();
    assert(std::strcmp(registry.getActiveName(), "Synthwave") == 0);
    registry.onNoteOn(64, 110);
    oled.render(engine, 0.75f);
    assert(oled.getDisplay()->getDrawCallCount() > 0);

    // Queued steps (from the USB MIDI task) apply on the next render, wrapping around
    oled.requestAnimationStep(1);
    assert(std::strcmp(registry.getActiveName(), "Synthwave") == 0); // not applied until render
    oled.render(engine, 0.75f);
    assert(std::strcmp(registry.getActiveName(), "Keyboard") == 0);
    oled.requestAnimationStep(-1);
    oled.requestAnimationStep(-1);
    oled.render(engine, 0.75f);
    assert(registry.getIndex() == 1);

    std::cout << "  -> OledDisplayUI & AnimationRegistry tests PASSED!" << std::endl;
}

// Captures the AnimationContext handed to the active animation each frame.
class ContextProbeAnimation : public HardwareDriver::IAnimation {
public:
    void init() override {}
    void reset() override {}
    void update(float, const HardwareDriver::AnimationContext& ctx) override { lastCtx = ctx; }
    void render(U8G2&, const HardwareDriver::AnimationContext&) override {}
    const char* getName() const override { return "Probe"; }
    HardwareDriver::AnimationContext lastCtx{};
};

void test_oled_keybed_offset_plumbing() {
    std::cout << "[Test] Running OLED keybed note offset plumbing test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);
    HardwareDriver::AnimationRegistry registry;
    ContextProbeAnimation probe;
    registry.registerAnimation(&probe);
    HardwareDriver::OledDisplayUI oled;
    assert(oled.init());
    oled.setAnimationRegistry(&registry);

    oled.render(engine, 0.5f);
    assert(probe.lastCtx.keybedNoteOffset == 0); // default: centre octave, no transpose

    oled.setKeybedNoteOffset(-22);
    oled.render(engine, 0.5f);
    assert(probe.lastCtx.keybedNoteOffset == -22);

    std::cout << "  -> OLED keybed note offset plumbing tests PASSED!" << std::endl;
}

void test_road_racer() {
    std::cout << "[Test] Running Road Racer test..." << std::endl;
    using namespace HardwareDriver;
    const float dt = 0.05f;
    AnimationContext ctx{};

    // Steering: a higher key than the last one steers right, a lower one left
    RoadRacerAnimation car;
    car.init();
    ctx.activeVoiceCount = 1;
    ctx.lastNoteOnMillis = 1000;
    car.onNoteOn(60, 100);
    car.update(dt, ctx);
    assert(car.getSteer() == 0.0f);          // first note has nothing to compare against
    ctx.lastNoteOnMillis = 1300;
    car.onNoteOn(67, 100);
    car.update(dt, ctx);
    assert(car.getSteer() > 0.5f);
    for (int i = 0; i < 6; ++i) car.update(dt, ctx);
    assert(car.getPlayerX() > 0.05f);         // still on the opening straight, so this is steering

    RoadRacerAnimation left;
    left.init();
    ctx.lastNoteOnMillis = 1000;
    left.onNoteOn(64, 100);
    left.update(dt, ctx);
    ctx.lastNoteOnMillis = 1300;
    left.onNoteOn(60, 100);
    for (int i = 0; i < 6; ++i) left.update(dt, ctx);
    assert(left.getPlayerX() < -0.05f);

    // Chord: notes within the chord window don't steer
    RoadRacerAnimation chord;
    chord.init();
    ctx.lastNoteOnMillis = 1000;
    chord.onNoteOn(60, 100);
    chord.update(dt, ctx);
    ctx.lastNoteOnMillis = 1020;
    chord.onNoteOn(67, 100);
    chord.update(dt, ctx);
    assert(chord.getSteer() == 0.0f);

    // Speed: more sounding voices = faster
    RoadRacerAnimation one, three;
    one.init();
    three.init();
    AnimationContext oneCtx{}, threeCtx{};
    oneCtx.activeVoiceCount = 1;
    threeCtx.activeVoiceCount = 3;
    for (int i = 0; i < 40; ++i) {
        one.update(dt, oneCtx);
        three.update(dt, threeCtx);
    }
    assert(three.getSpeed() > one.getSpeed() + 0.3f);

    // Render stays inside the blue zone and draws the car
    U8G2 d;
    d.begin();
    three.render(d, threeCtx);
    for (int x = 0; x < 128; ++x) {
        for (int y = 0; y < ANIM_REGION_TOP; ++y) assert(!d.getPixel(x, y));
        assert(!d.getPixel(x, 63));
    }
    assert(d.getPixel(RoadRacerAnimation::CAR_X, RoadRacerAnimation::CAR_BOTTOM - 4)); // car body

    std::cout << "  -> Road Racer tests PASSED!" << std::endl;
}

static bool blueZoneOnly(const U8G2& d) {
    for (int x = 0; x < 128; ++x) {
        for (int y = 0; y < HardwareDriver::ANIM_REGION_TOP; ++y) if (d.getPixel(x, y)) return false;
        if (d.getPixel(x, 63)) return false;
    }
    return true;
}

void test_synthwave() {
    std::cout << "[Test] Running Synthwave test..." << std::endl;
    using namespace HardwareDriver;
    SynthwaveAnimation a;
    a.init();
    AnimationContext ctx{};
    ctx.bpm = 120.0f;

    // Free-running grid: 120 BPM x 2 lines per beat = 4 lines/s, so 0.1 s moves 0.4 of a line
    a.update(0.1f, ctx);
    assert(std::fabs(a.getGridPhase() - 0.4f) < 0.01f);
    // Metronome on: locked to the beat phase
    ctx.metronomeOn = true;
    ctx.beatPhase = 0.25f;
    a.update(0.05f, ctx);
    assert(std::fabs(a.getGridPhase() - 0.5f) < 0.01f);

    // A note pulses the sun, which then decays back
    const float rest = a.getSunRadius();
    a.onNoteOn(60, 127);
    assert(a.getSunRadius() > rest + 3.0f);
    for (int i = 0; i < 40; ++i) a.update(0.05f, ctx);
    assert(a.getSunRadius() < rest + 0.5f);

    // A chord raises the mountains
    const float low = a.getMountainScale();
    ctx.voiceEnvelopes[0] = ctx.voiceEnvelopes[1] = ctx.voiceEnvelopes[2] = 1.0f;
    for (int i = 0; i < 20; ++i) a.update(0.05f, ctx);
    assert(a.getMountainScale() > low + 0.5f);

    U8G2 d;
    d.begin();
    a.render(d, ctx);
    assert(blueZoneOnly(d));
    assert(d.getPixel(SynthwaveAnimation::SUN_X, SynthwaveAnimation::HORIZON_Y - 8)); // sun body
    assert(d.getPixel(0, SynthwaveAnimation::HORIZON_Y));                            // horizon line

    std::cout << "  -> Synthwave tests PASSED!" << std::endl;
}

void test_lissajous() {
    std::cout << "[Test] Running Lissajous test..." << std::endl;
    using namespace HardwareDriver;
    char name[12];
    auto named = [&](std::initializer_list<uint8_t> notes) {
        std::vector<uint8_t> v(notes);
        LissajousAnimation::chordName(v.data(), static_cast<int>(v.size()), name, sizeof(name));
        return std::string(name);
    };
    assert(named({60}) == "C4");
    assert(named({60, 67}) == "P5");
    assert(named({60, 63}) == "m3");
    assert(named({60, 72}) == "P8");
    assert(named({60, 64, 67}) == "C MAJ");
    assert(named({64, 67, 72}) == "C MAJ");      // first inversion
    assert(named({57, 60, 64}) == "A MIN");
    assert(named({55, 59, 62, 65}) == "G 7");
    assert(named({60, 61, 66}) == "");           // cluster: no name

    LissajousAnimation a;
    a.init();
    AnimationContext ctx{};
    U8G2 d;
    d.begin();

    // Idle: a flat beam on the centre row, nothing far above or below it
    a.update(0.05f, ctx);
    a.render(d, ctx);
    assert(blueZoneOnly(d));
    assert(a.getVoiceCount() == 0);
    for (int x = 0; x < 128; x += 3) assert(!d.getPixel(x, LissajousAnimation::CENTRE_Y - 10));

    // One loud note: the figure spans most of the screen width
    ctx.voiceNotes[3] = 60;
    ctx.voiceEnvelopes[3] = 1.0f;
    a.update(0.05f, ctx);
    d.clearBuffer();
    a.render(d, ctx);
    assert(blueZoneOnly(d));
    assert(a.getVoiceCount() == 1);
    assert(std::string(a.getLabel()) == "C4");
    int minX = 128, maxX = -1;
    for (int x = 0; x < 128; ++x) {
        for (int y = ANIM_REGION_TOP; y <= ANIM_REGION_BOTTOM; ++y) {
            // ignore the graticule's centre cross and the label corner
            if (y == LissajousAnimation::CENTRE_Y || x == LissajousAnimation::CENTRE_X || (x > 100 && y > 54)) continue;
            if (d.getPixel(x, y) && (y % 8 != (LissajousAnimation::CENTRE_Y % 8) || x % 16 != 0)) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
            }
        }
    }
    assert(maxX - minX > 60);

    // Released voices drop out
    ctx.voiceEnvelopes[3] = 0.0f;
    a.update(0.05f, ctx);
    assert(a.getVoiceCount() == 0);

    std::cout << "  -> Lissajous tests PASSED!" << std::endl;
}

void test_keyboard_animation_render() {
    std::cout << "[Test] Running KeyboardAnimation render test..." << std::endl;
    using namespace HardwareDriver;
    using namespace HardwareDriver::KeyboardLayout;

    auto ctxWith = [](int offset) {
        AnimationContext c{};
        c.keybedNoteOffset = static_cast<int16_t>(offset);
        return c;
    };
    // Interior probe points: bottom of a white key's inner area (below any black key), and
    // the centre of a black key's inner area.
    auto whiteLit = [](U8G2& d, int key) {
        const int x = whiteKeyX(whiteColumn(key));
        return d.getPixel(x + 4, KEY_TOP + WHITE_H - 3);
    };
    auto blackLit = [](U8G2& d, int key) {
        return d.getPixel(blackKeyX(key) + BLACK_W / 2, KEY_TOP + BLACK_H - 4);
    };
    auto rowsOutsideBlueZoneEmpty = [](U8G2& d) {
        for (int x = 0; x < 128; ++x) {
            for (int y = 0; y < 16; ++y) if (d.getPixel(x, y)) return false;
            for (int y = 63; y < 64; ++y) if (d.getPixel(x, y)) return false;
        }
        return true;
    };
    auto render = [&](KeyboardAnimation& a, U8G2& d, const AnimationContext& ctx) {
        d.clearBuffer();
        a.render(d, ctx);
    };

    U8G2 d;
    d.begin();

    // 1. Idle: outlines only, nothing filled, every key present
    KeyboardAnimation kb;
    kb.init();
    AnimationContext ctx = ctxWith(0);
    kb.update(0.02f, ctx);
    render(kb, d, ctx);
    for (int k = 0; k < KEY_COUNT; ++k) {
        assert(!(isBlackKey(k) ? blackLit(d, k) : whiteLit(d, k)));
    }
    assert(d.getPixel(whiteKeyX(0), KEY_TOP + 10));                 // left border of key 0
    assert(d.getPixel(whiteKeyX(15), KEY_TOP + 10));                // right border of the top C
    assert(d.getPixel(blackKeyX(1), KEY_TOP + 5));                  // C# frame is drawn
    assert(rowsOutsideBlueZoneEmpty(d));

    // 2. Middle C (note 60) at centre octave lights physical key 12 only
    ctx = ctxWith(0);
    ctx.voiceNotes[0] = 60;
    ctx.voiceEnvelopes[0] = 1.0f;
    kb.update(0.02f, ctx);
    render(kb, d, ctx);
    assert(whiteLit(d, 12));
    assert(!whiteLit(d, 0) && !whiteLit(d, 14) && !whiteLit(d, 11 + 1 + 2));
    assert(rowsOutsideBlueZoneEmpty(d));

    // 3. Same note after OCT+ (offset +12) is physical key 0, not key 12
    AnimationContext up = ctxWith(12);
    up.voiceNotes[0] = 60;
    up.voiceEnvelopes[0] = 1.0f;
    KeyboardAnimation kb2;
    kb2.update(0.02f, up);
    render(kb2, d, up);
    assert(whiteLit(d, 0));
    assert(!whiteLit(d, 12));

    // 4. A black key (note 49 = C#3, key 1) fills
    AnimationContext blk = ctxWith(0);
    blk.voiceNotes[3] = 49;
    blk.voiceEnvelopes[3] = 1.0f;
    KeyboardAnimation kb3;
    kb3.update(0.02f, blk);
    render(kb3, d, blk);
    assert(blackLit(d, 1));
    assert(!blackLit(d, 3));

    // 5. Fill height tracks the envelope: half level leaves the top half of the key dark
    AnimationContext half = ctxWith(0);
    half.voiceNotes[0] = 48;
    half.voiceEnvelopes[0] = 0.5f;
    KeyboardAnimation kb4;
    kb4.update(0.02f, half);
    render(kb4, d, half);
    const int x0 = whiteKeyX(0) + 4;
    assert(d.getPixel(x0, KEY_TOP + WHITE_H - 3));                  // near bottom: filled
    assert(!d.getPixel(x0, KEY_TOP + 3));                           // near top: empty

    // 6. Notes off the physical keybed draw nothing extra (identical to idle)
    KeyboardAnimation idleKb, offKb;
    AnimationContext idle = ctxWith(0), off = ctxWith(0);
    off.voiceNotes[0] = 20;  off.voiceEnvelopes[0] = 1.0f;
    off.voiceNotes[1] = 100; off.voiceEnvelopes[1] = 1.0f;
    idleKb.update(0.02f, idle);
    offKb.update(0.02f, off);
    render(idleKb, d, idle);
    std::vector<uint8_t> idleFb(d.getFramebuffer(), d.getFramebuffer() + 128 * 64);
    render(offKb, d, off);
    assert(std::equal(idleFb.begin(), idleFb.end(), d.getFramebuffer()));

    // 7. Release: after the voice ends the key stays lit while its envelope decays, then
    //    goes fully dark once the level reaches zero
    KeyboardAnimation kb5;
    AnimationContext rel = ctxWith(0);
    rel.voiceNotes[0] = 60;
    rel.voiceEnvelopes[0] = 1.0f;
    kb5.update(0.02f, rel);
    rel.voiceEnvelopes[0] = 0.0f;  // voice slot goes inactive
    rel.voiceNotes[0] = 0;
    kb5.update(0.02f, rel);
    render(kb5, d, rel);
    assert(whiteLit(d, 12));                                        // still fading
    for (int i = 0; i < 200; ++i) kb5.update(0.02f, rel);
    render(kb5, d, rel);
    assert(!whiteLit(d, 12));                                       // fully off

    std::cout << "  -> KeyboardAnimation render tests PASSED!" << std::endl;
}

void test_keyboard_layout() {
    std::cout << "[Test] Running MK4 25-key layout mapping test..." << std::endl;
    using namespace HardwareDriver::KeyboardLayout;

    // Default octave: C3 (48) .. C5 (72) -> physical keys 0..24
    assert(keyIndexForNote(48, 0) == 0);
    assert(keyIndexForNote(60, 0) == 12);
    assert(keyIndexForNote(72, 0) == 24);
    assert(keyIndexForNote(47, 0) == -1);
    assert(keyIndexForNote(73, 0) == -1);

    // OCT+ (offset +12): the MPK now sends 60..84 for keys 0..24
    assert(keyIndexForNote(60, 12) == 0);
    assert(keyIndexForNote(84, 12) == 24);
    assert(keyIndexForNote(48, 12) == -1);
    // OCT- with transpose +2 (offset -12+2 = -10): note 38 is key 0
    assert(keyIndexForNote(38, -10) == 0);
    // Out-of-range MIDI notes never map
    assert(keyIndexForNote(-5, 0) == -1);
    assert(keyIndexForNote(200, 0) == -1);

    // Black/white pattern C C# D D# E F F# G G# A A# B, repeating, ending on a white C
    const bool black[25] = {0,1,0,1,0, 0,1,0,1,0,1,0, 0,1,0,1,0, 0,1,0,1,0,1,0, 0};
    int whites = 0, blacks = 0;
    for (int k = 0; k < KEY_COUNT; ++k) {
        assert(isBlackKey(k) == black[k]);
        (black[k] ? blacks : whites)++;
    }
    assert(whites == 15 && blacks == 10);

    // White-key column index: C=0 D=1 E=2 F=3 ... top C = 14
    assert(whiteColumn(0) == 0);
    assert(whiteColumn(2) == 1);
    assert(whiteColumn(4) == 2);
    assert(whiteColumn(5) == 3);
    assert(whiteColumn(24) == 14);
    // A black key sits on the boundary right of the white column to its left: C# -> col 0
    assert(whiteColumn(1) == 0);
    assert(whiteColumn(13) == 7);

    std::cout << "  -> MK4 25-key layout mapping tests PASSED!" << std::endl;
}

static bool drewString(const U8G2* d, const char* text) {
    const auto& v = d->getDrawnStrings();
    return std::find(v.begin(), v.end(), std::string(text)) != v.end();
}

void test_system_status_faults() {
    std::cout << "[Test] Running SystemStatus fault set/clear & MPK link watcher test..." << std::endl;
    SystemStatus st;
    assert(st.mask() == 0 && !st.hasFatal());

    // Set logs once; setting again is a no-op; clear logs once
    st.set(SystemFault::Ble, 100);
    st.set(SystemFault::Ble, 200);
    assert(st.has(SystemFault::Ble) && !st.hasFatal());
    assert(st.getLogCount() == 1);
    st.clear(SystemFault::Ble);
    st.clear(SystemFault::Ble);
    assert(!st.has(SystemFault::Ble) && st.getLogCount() == 2);

    // Fatal vs warning classification and priority
    st.set(SystemFault::Psram, 0);
    st.set(SystemFault::AudioTask, 0);
    st.set(SystemFault::AudioDac, 0);
    assert(st.hasFatal());
    assert(SystemStatus::first(st.fatalMask()) == SystemFault::AudioDac);
    assert(SystemStatus::first(st.warningMask()) == SystemFault::Psram);
    assert(SystemStatus::countBits(st.fatalMask()) == 2);
    for (int i = 0; i < static_cast<int>(SystemFault::Count); ++i) {
        const FaultInfo& fi = SystemStatus::info(static_cast<SystemFault>(i));
        const bool isFatal = ((SystemStatus::FATAL_MASK >> i) & 1u) != 0;
        assert(fi.fatal == isFatal);
        // Fit the 128 px screen: 21 chars of 6x10, tag <= 8 and banner <= 25 chars of 5x7
        assert(std::strlen(fi.title) <= 21 && std::strlen(fi.hint1) <= 21 && std::strlen(fi.hint2) <= 21);
        assert(std::strlen(fi.tag) <= 8 && std::strlen(fi.banner) <= 25);
        assert(fi.log && fi.log[0] != '\0');
        if (!fi.fatal) assert(fi.tag[0] != '\0' && fi.banner[0] != '\0');
        else assert(fi.title[0] != '\0' && fi.hint1[0] != '\0');
    }

    // Banner: shown for 3 s after a warning is raised, cancelled when that warning clears
    SystemStatus b;
    SystemFault bf = SystemFault::Count;
    assert(!b.bannerFault(0, bf));
    b.set(SystemFault::Nvs, 1000);
    assert(b.bannerFault(1000, bf) && bf == SystemFault::Nvs);
    assert(b.bannerFault(3999, bf));
    assert(!b.bannerFault(4000, bf));
    b.set(SystemFault::Ble, 5000);
    b.clear(SystemFault::Ble);
    assert(!b.bannerFault(5001, bf));
    b.set(SystemFault::AudioDac, 6000); // fatal faults use the fault page, not the banner
    assert(!b.bannerFault(6001, bf));

    // MPK link watcher: waiting page at boot, MpkMissing after 5 s without enumeration
    SystemStatus m;
    assert(!m.isWaitingForMpk()); // watcher not started
    m.updateMpkLink(1000, false, 0);
    assert(m.isWaitingForMpk() && !m.has(SystemFault::MpkMissing));
    m.updateMpkLink(1000 + SystemStatus::MPK_ENUM_TIMEOUT_MS - 1, false, 0);
    assert(!m.has(SystemFault::MpkMissing));
    m.updateMpkLink(1000 + SystemStatus::MPK_ENUM_TIMEOUT_MS, false, 0);
    assert(m.has(SystemFault::MpkMissing) && m.isWaitingForMpk());

    // Enumerates: MpkMissing clears, waiting page ends
    m.updateMpkLink(7000, true, 0);
    assert(!m.has(SystemFault::MpkMissing) && !m.isWaitingForMpk());
    assert(!m.has(SystemFault::PluginPortOffline));
    // Replies keep arriving: Plugin Port stays online
    m.updateMpkLink(9000, true, 5);
    m.updateMpkLink(11500, true, 12);
    assert(!m.has(SystemFault::PluginPortOffline));
    // Replies stop for 3 s: Plugin Port offline, then back online on the next reply
    m.updateMpkLink(11500 + SystemStatus::PLUGIN_PORT_TIMEOUT_MS - 1, true, 12);
    assert(!m.has(SystemFault::PluginPortOffline));
    m.updateMpkLink(11500 + SystemStatus::PLUGIN_PORT_TIMEOUT_MS, true, 12);
    assert(m.has(SystemFault::PluginPortOffline));
    m.updateMpkLink(15000, true, 13);
    assert(!m.has(SystemFault::PluginPortOffline));

    // Unplugged at runtime: MpkMissing immediately (no boot timeout), Plugin Port fault dropped, no waiting page
    m.updateMpkLink(18000, true, 13);
    assert(m.has(SystemFault::PluginPortOffline));
    m.updateMpkLink(18050, false, 13);
    assert(m.has(SystemFault::MpkMissing) && !m.has(SystemFault::PluginPortOffline));
    assert(!m.isWaitingForMpk());
    assert(m.bannerFault(18050, bf) && bf == SystemFault::MpkMissing);
    // Replugged: the stale count must not trip the Plugin Port timeout straight away
    m.updateMpkLink(30000, true, 13);
    assert(!m.has(SystemFault::MpkMissing) && !m.has(SystemFault::PluginPortOffline));

    // UsbHostMidi counts inbound Akai SysEx only (feeds the Plugin Port watcher)
    Synth::SynthEngine engine;
    engine.init(44100);
    UsbHostMidi usb;
    usb.init(engine);
    assert(!usb.isDeviceConnected());
    assert(usb.getMpkSysExCount() == 0);
    const uint8_t keybedReply[] = {0xF0, 0x47, 0x00, 0x5D, 0x6A, 0x00, 0x02, 0x00, 0x00, 0xF7};
    const uint8_t otherSysEx[] = {0xF0, 0x7D, 0x01, 0x00, 0xF7};
    usb.processSysEx(keybedReply, sizeof(keybedReply));
    usb.processSysEx(otherSysEx, sizeof(otherSysEx));
    assert(usb.getMpkSysExCount() == 1);

    // BLE / NVS health flags (mock NVS always opens; the BLE controller check is ESP32-only)
    Preferences::clearAllStorage();
    BleMidiServer ble;
    assert(ble.init(engine));
    assert(ble.isNvsOk() && ble.isBleOk());

    std::cout << "  -> SystemStatus fault model tests PASSED!" << std::endl;
}

void test_oled_fault_pages() {
    std::cout << "[Test] Running OLED fault page / waiting page / warning tag test..." << std::endl;
    setMockMillis(10000);
    Synth::SynthEngine engine;
    engine.init(44100);
    AnimationRegistry registry;
    ContextProbeAnimation probe;
    registry.registerAnimation(&probe);

    OledDisplayUI oled;
    assert(oled.init());
    oled.setAnimationRegistry(&registry);
    oled.setPreset(0, "JUNO-60");
    SystemStatus st;
    oled.setSystemStatus(&st);
    U8G2* d = oled.getDisplay();

    // No faults, watcher not started: normal UI, no fault text
    oled.render(engine, 0.5f);
    assert(drewString(d, "P1 JUNO-60"));
    assert(!drewString(d, "Connect") && !drewString(d, "NO AUDIO"));

    // Boot: MK4 not yet enumerated -> waiting page in the blue zone, header stays
    st.updateMpkLink(10000, false, 0);
    oled.render(engine, 0.5f);
    assert(drewString(d, "P1 JUNO-60"));
    assert(drewString(d, "Connect") && drewString(d, "MPK Mini MK4") && drewString(d, "to the USB host port"));
    assert(!drewString(d, "NO MPK"));
    // After the enumeration timeout the hint changes and the header tag appears; no banner over the page
    setMockMillis(15000);
    st.updateMpkLink(15000, false, 0);
    oled.render(engine, 0.5f);
    assert(drewString(d, "not found - check cable") && drewString(d, "NO MPK"));
    assert(!drewString(d, "MPK Mini MK4 unplugged"));

    // MK4 enumerates: waiting page and tag clear
    setMockMillis(16000);
    st.updateMpkLink(16000, true, 0);
    oled.render(engine, 0.5f);
    assert(!drewString(d, "Connect") && !drewString(d, "NO MPK"));
    assert(drewString(d, "P1 JUNO-60"));
    assert(!d->getPixel(127, 8));

    // Plugin Port silent for 3 s: header tag + banner, banner gone after 3 s, tag stays until resolved
    setMockMillis(19000);
    st.updateMpkLink(19000, true, 0);
    assert(st.has(SystemFault::PluginPortOffline));
    oled.render(engine, 0.5f);
    assert(drewString(d, "PORT OFF") && drewString(d, "MPK Plugin Port offline"));
    assert(d->getPixel(127, 8));  // inverted tag box under the header stats
    assert(d->getPixel(0, 53));   // banner box at the bottom of the blue zone
    setMockMillis(22500);
    st.updateMpkLink(22500, true, 0);
    oled.render(engine, 0.5f);
    assert(drewString(d, "PORT OFF") && !drewString(d, "MPK Plugin Port offline"));
    // Two warnings: highest priority tag plus "+", banner for the newest
    st.set(SystemFault::Ble, 22500);
    oled.render(engine, 0.5f);
    assert(drewString(d, "PORT OFF+") && drewString(d, "BLE patch link off"));
    st.clear(SystemFault::Ble);
    setMockMillis(23000);
    st.updateMpkLink(23000, true, 1); // a reply arrives
    oled.render(engine, 0.5f);
    assert(!drewString(d, "PORT OFF") && !drewString(d, "PORT OFF+"));
    assert(!d->getPixel(127, 8));

    // Fatal: full-screen fault page replaces header and animation, sent once while unchanged
    st.set(SystemFault::AudioDac, 23000);
    oled.render(engine, 0.5f);
    assert(drewString(d, "NO AUDIO") && drewString(d, "I2S DAC init failed") &&
           drewString(d, "check PCM5102A wiring") && drewString(d, "details: serial log"));
    assert(!drewString(d, "P1 JUNO-60"));
    assert(d->getPixel(0, 0) && d->getPixel(127, 14)); // inverted title banner fills the yellow zone
    const uint32_t sent = d->getBufferSentCount();
    oled.render(engine, 0.5f);
    oled.render(engine, 0.5f);
    assert(d->getBufferSentCount() == sent);
    // A second fatal fault redraws once and points at the serial log
    st.set(SystemFault::AudioTask, 23000);
    oled.render(engine, 0.5f);
    assert(d->getBufferSentCount() == sent + 1);
    assert(drewString(d, "NO AUDIO") && drewString(d, "more faults: serial log"));

    // Resolved: normal UI returns
    st.clear(SystemFault::AudioDac);
    st.clear(SystemFault::AudioTask);
    oled.render(engine, 0.5f);
    assert(drewString(d, "P1 JUNO-60") && !drewString(d, "NO AUDIO"));
    assert(d->getBufferSentCount() == sent + 2);

    g_useMockMillisOverride = false;
    std::cout << "  -> OLED fault page tests PASSED!" << std::endl;
}

// A note-on for a key already down means the MPK's note-off for it never arrived (a stuck note on
// a mono patch); a note-off for a key that is up means its note-on was lost. Both are counted.
void test_usb_lost_keybed_event_counters() {
    std::cout << "[Test] Running UsbHostMidi lost keybed event counters test..." << std::endl;
    Synth::SynthEngine engine;
    engine.init(44100);
    UsbHostMidi usb;
    usb.init(engine);

    usb.processMidiMessage(0x90, 60, 100);
    usb.processMidiMessage(0x90, 64, 100);
    usb.processMidiMessage(0x80, 60, 0);
    usb.processMidiMessage(0x90, 64, 0); // note-off as note-on velocity 0
    assert(usb.getLostNoteOffCount() == 0);
    assert(usb.getLostNoteOnCount() == 0);

    usb.processMidiMessage(0x90, 73, 100);
    usb.processMidiMessage(0x90, 73, 90); // note-off for 73 was lost
    assert(usb.getLostNoteOffCount() == 1);
    usb.processMidiMessage(0x80, 57, 0);  // note-on for 57 was lost
    assert(usb.getLostNoteOnCount() == 1);
    std::cout << "  -> Lost keybed event counters PASSED!" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "  Running ESP32-S3 Rev 2.0 Driver Unit Tests      " << std::endl;
    std::cout << "==================================================" << std::endl;

    test_mpk_knob_pre_seeding();
    test_mpk_bank_select();
    test_ble_midi_and_nvs_persistence();
    test_usb_host_midi();
    test_usb_lost_keybed_event_counters();
    test_oled_ui();
    test_keyboard_layout();
    test_oled_keybed_offset_plumbing();
    test_keyboard_animation_render();
    test_road_racer();
    test_synthwave();
    test_lissajous();
    test_system_status_faults();
    test_oled_fault_pages();

    std::cout << "\n>>> ALL REVISION 2.0 DRIVER TESTS PASSED (12/12)! <<<" << std::endl;
    return 0;
}
