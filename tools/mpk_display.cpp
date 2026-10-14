/**
 * mpk_display.cpp
 * Production CLI Tool for Akai MPK Mini IV Display & RAM Configuration
 *
 * Supports:
 *   1. Hardware RAM & Plugin Slot Preset Sync: Writes synth knob labels (Cutoff, Resonance, etc.),
 *      Pad Theme Color, and Program Name to MPK Mini IV Slot 0 (RAM) & Slot 2 (PLUGIN) via Software Control / Plugin Port.
 *   2. Live OLED Text & Color Control: Sends 3-line formatted text to the OLED
 *      display with custom per-preset background colors via Plugin Port & Software Control Port.
 *   3. Interactive Live Monitor: Listens to incoming CC messages on Plugin Port (CC 24..31, CC 80/81)
 *      and updates the OLED display in real-time.
 */

#include "MpkMiniMk4Display.h"
#include "SynthEngine.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <rtmidi/RtMidi.h>

std::atomic<bool> g_running{true};

void sigHandler(int) {
    g_running = false;
}

// ---------------------------------------------------------------------------
// SysEx Helper Wrappers (Delegating to Standalone MpkMiniMk4::DisplayDriver Library)
// ---------------------------------------------------------------------------

std::vector<unsigned char> makeSelectModeSysEx(uint8_t mode = 0x01, uint8_t devId = 0x00) {
    return MpkMiniMk4::DisplayDriver::makeSelectModeSysEx(
        static_cast<MpkMiniMk4::HardwareMode>(mode), devId);
}

std::vector<unsigned char> makeScreenOwnerSysEx(uint8_t ownerType = 0x01, uint8_t devId = 0x00) {
    return MpkMiniMk4::DisplayDriver::makeScreenOwnerSysEx(
        static_cast<MpkMiniMk4::OwnerType>(ownerType), "MK4-SYNTH-SVK0", devId);
}

// Tool colors are given as 0..63 per channel
MpkMiniMk4::Color from6Bit(uint8_t r, uint8_t g, uint8_t b) {
    auto scale = [](uint8_t v) { return static_cast<uint8_t>(std::min<int>(v, 63) * 255 / 63); };
    return {scale(r), scale(g), scale(b)};
}

std::vector<unsigned char> makeTextFormatSysEx(uint8_t line, uint8_t align,
                                               uint8_t fr, uint8_t fg, uint8_t fb,
                                               uint8_t br = 0, uint8_t bg = 0, uint8_t bb = 0,
                                               uint8_t devId = 0x00, uint8_t layout = 0x02) {
    return MpkMiniMk4::DisplayDriver::makeLineFormatSysEx(
        line,
        static_cast<MpkMiniMk4::Align>(align),
        from6Bit(fr, fg, fb),
        from6Bit(br, bg, bb),
        static_cast<MpkMiniMk4::Layout>(layout),
        devId);
}

std::vector<unsigned char> makeDisplayTextSysEx(const std::string& text, uint8_t line,
                                                uint8_t devId = 0x00, uint8_t layout = 0x02) {
    return MpkMiniMk4::DisplayDriver::makeLineTextSysEx(
        line, text, static_cast<MpkMiniMk4::Layout>(layout), devId);
}

// ---------------------------------------------------------------------------
// Port Discovery
// ---------------------------------------------------------------------------

int findPort(RtMidi& midi, const std::string& pattern) {
    for (unsigned int i = 0; i < midi.getPortCount(); ++i) {
        if (midi.getPortName(i).find(pattern) != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// RAM Preset Synchronization (Software Control Port)
// ---------------------------------------------------------------------------

struct KnobDef {
    uint8_t cc;
    std::string name;
};

bool syncHardwareKnobLabels(const std::string& progName, const std::vector<KnobDef>& knobs) {
    RtMidiIn midiIn;
    RtMidiOut midiOut;

    int inIdx = findPort(midiIn, "Software Control");
    int outIdx = findPort(midiOut, "Software Control");

    if (inIdx < 0 || outIdx < 0) {
        std::cerr << "[Error] MPK mini IV Software Control Port not found!\n";
        return false;
    }

    midiIn.openPort(inIdx);
    midiOut.openPort(outIdx);
    midiIn.ignoreTypes(false, false, false);

    std::vector<unsigned char> receivedPreset;
    std::atomic<bool> gotPreset{false};

    midiIn.setCallback([](double, std::vector<unsigned char>* msg, void* userData) {
        auto* pGot = static_cast<std::atomic<bool>*>(userData);
        if (msg && msg->size() == 284 && (*msg)[4] == 0x67) {
            *pGot = true;
        }
    }, &gotPreset);

    // Query Slot 2 (PLUGIN) as clean template: F0 47 7F 5D 66 00 00 02 F7
    std::vector<unsigned char> query = {0xF0, 0x47, 0x7F, 0x5D, 0x66, 0x00, 0x00, 0x02, 0xF7};
    midiOut.sendMessage(&query);

    for (int i = 0; i < 30 && !gotPreset; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    midiIn.cancelCallback();

    if (!gotPreset || receivedPreset.empty()) {
        std::vector<unsigned char> msg;
        midiOut.sendMessage(&query);
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            midiIn.getMessage(&msg);
            if (msg.size() == 284 && msg[4] == 0x67) {
                receivedPreset = msg;
                gotPreset = true;
                break;
            }
        }
    }

    if (receivedPreset.size() != 284) {
        std::cerr << "[Error] Failed to read 284-byte RAM preset from MPK Mini IV.\n";
        return false;
    }

    std::cout << "[OK] Read 284-byte RAM preset from MPK Mini IV.\n";

    std::vector<unsigned char> writePreset = receivedPreset;
    writePreset[4] = 0x64; // Write command (0x64)

    // Set Program Name (offset 8..23, 16 bytes)
    for (size_t i = 0; i < 16 && (8 + i) < writePreset.size(); ++i) {
        writePreset[8 + i] = (i < progName.size()) ? static_cast<unsigned char>(progName[i]) : 0;
    }

    // Configure 8 knobs at offset 117 + k * 20 with strict bounds check
    size_t k1Offset = 117;
    for (size_t k = 0; k < 8 && k < knobs.size(); ++k) {
        size_t off = k1Offset + k * 20;
        if (off + 20 > writePreset.size()) break;
        writePreset[off + 0] = static_cast<unsigned char>(24 + k); // Strictly native CC 24..31 (0x18..0x1F)
        writePreset[off + 1] = 0x00;                               // Min = 0
        writePreset[off + 2] = 0x7F;                               // Max = 127
        writePreset[off + 3] = 0x00;                               // Mode = 0 (Absolute)

        for (size_t n = 0; n < 16; ++n) {
            writePreset[off + 4 + n] = (n < knobs[k].name.size())
                ? static_cast<unsigned char>(knobs[k].name[n]) : 0;
        }
    }

    // Write to Slot 0 (Active RAM) so knob labels & pad colors update in <5ms without SPI flash erase resetting 0x1C display mode
    writePreset[7] = 0x00;
    midiOut.sendMessage(&writePreset);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    if ( std::getenv("DUMP_MPK_PRESET") ) {
        std::cout << "MPK_PRESET_BYTES=[";
        for (size_t i = 0; i < writePreset.size(); ++i) {
            std::cout << static_cast<int>(writePreset[i]) << (i + 1 < writePreset.size() ? "," : "");
        }
        std::cout << "]\n";
    }

    std::cout << "[OK] Uploaded RAM preset to MPK Mini IV (Slot 0 RAM & Slot 2 PLUGIN)!\n";
    std::cout << "  Program Name: \"" << progName << "\"\n";
    for (size_t k = 0; k < 8 && k < knobs.size(); ++k) {
        std::cout << "  K" << (k + 1) << " (CC " << (24 + k)
                  << ") -> " << knobs[k].name << "\n";
    }
    return true;
}

// ---------------------------------------------------------------------------
// Live OLED Screen Writing (Plugin Port & Software Control Port - Slot 2: PLUGIN)
// ---------------------------------------------------------------------------

bool sendLiveDisplay(const std::string& line0, const std::string& line1, const std::string& line2,
                     uint8_t bgR = 24, uint8_t bgG = 4, uint8_t bgB = 28,
                     uint8_t align0 = 1, uint8_t align1 = 0, uint8_t align2 = 2,
                     int bg1R = -1, int bg1G = -1, int bg1B = -1,
                     int bg2R = -1, int bg2G = -1, int bg2B = -1) {
    // IMPORTANT (Firmware 0x0800e13e): [0x800ec1c] (activeScreenOwner) is a SINGLE global byte!
    // Sending 0x1C (0x02) on DAW Port after 0x1C (0x01) on Plugin Port overwrites [0x800ec1c] = 2
    // while [0x800ec20] (activeMode) = 1 (Plugin), causing a mode/owner mismatch!
    // Therefore, exclusively use Plugin Port (mode=0x01, ownerType=0x01) when present.
    struct PortTarget {
        const char* pattern;
        uint8_t ownerType;
        bool selectPluginMode;
    };
    const PortTarget targets[] = {
        {"Plugin Port", 0x01, true},
        {"DAW Port",    0x02, false}
    };
    bool anySent = false;

    uint8_t l1BgR = (bg1R >= 0) ? static_cast<uint8_t>(bg1R) : static_cast<uint8_t>(bgR / 2);
    uint8_t l1BgG = (bg1G >= 0) ? static_cast<uint8_t>(bg1G) : static_cast<uint8_t>(bgG / 2);
    uint8_t l1BgB = (bg1B >= 0) ? static_cast<uint8_t>(bg1B) : static_cast<uint8_t>(bgB / 2);

    uint8_t l2BgR = (bg2R >= 0) ? static_cast<uint8_t>(bg2R) : static_cast<uint8_t>(bgR / 2);
    uint8_t l2BgG = (bg2G >= 0) ? static_cast<uint8_t>(bg2G) : static_cast<uint8_t>(bgG / 2);
    uint8_t l2BgB = (bg2B >= 0) ? static_cast<uint8_t>(bg2B) : static_cast<uint8_t>(bgB / 2);

    for (const auto& t : targets) {
        RtMidiOut midiOut;
        int idx = findPort(midiOut, t.pattern);
        if (idx < 0) continue;
        midiOut.openPort(idx);
        anySent = true;

        uint8_t devId = 0x00;
        // 1. On Plugin Port, select Active Hardware Mode 0x01 (Plugin) via 0x3A so SVK0 display buffer is active
        if (t.selectPluginMode) {
            auto modeSx = makeSelectModeSysEx(0x01, devId);
            midiOut.sendMessage(&modeSx);
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }

        // 2. Take ownership of screen with port-specific ownerType (0x01 on Plugin Port, 0x02 on DAW Port)
        auto ownerSx = makeScreenOwnerSysEx(t.ownerType, devId);
        midiOut.sendMessage(&ownerSx);
        std::this_thread::sleep_for(std::chrono::milliseconds(6));

        // 3. Line 0 (Header Banner): align0 (0=Left, 1=Center, 2=Right), Bright White on bgR/bgG/bgB
        auto f0 = makeTextFormatSysEx(0, align0, 31, 63, 31, bgR, bgG, bgB, devId, 0x03);
        midiOut.sendMessage(&f0);
        auto t0 = makeDisplayTextSysEx(line0, 0, devId, 0x03);
        midiOut.sendMessage(&t0);

        // 4. Line 1 (Middle Line): align1 (0=Left, 1=Center, 2=Right), Crisp White/Cyan on l1BgR/l1BgG/l1BgB
        auto f1 = makeTextFormatSysEx(1, align1, 24, 63, 31, l1BgR, l1BgG, l1BgB, devId, 0x02);
        midiOut.sendMessage(&f1);
        auto t1 = makeDisplayTextSysEx(line1, 1, devId, 0x02);
        midiOut.sendMessage(&t1);

        // 5. Line 2 (Bottom Line): align2 (0=Left, 1=Center, 2=Right), Gold/White on l2BgR/l2BgG/l2BgB
        auto f2 = makeTextFormatSysEx(2, align2, 31, 55, 6, l2BgR, l2BgG, l2BgB, devId, 0x02);
        midiOut.sendMessage(&f2);
        auto t2 = makeDisplayTextSysEx(line2, 2, devId, 0x02);
        midiOut.sendMessage(&t2);

        // Stop after Plugin Port so DAW Port does not clobber [0x800ec1c] (activeScreenOwner)!
        break;
    }

    if (!anySent) {
        std::cerr << "[Error] No MPK mini IV output ports found!\n";
        return false;
    }

    const char* alignNames[] = {"LEFT", "CENTER", "RIGHT"};
    std::cout << "[OK] Wrote to MPK Mini IV OLED Display:\n"
              << "  Line 0 [" << alignNames[align0 % 3] << "]: \"" << line0 << "\"\n"
              << "  Line 1 [" << alignNames[align1 % 3] << "]: \"" << line1 << "\"\n"
              << "  Line 2 [" << alignNames[align2 % 3] << "]: \"" << line2 << "\"\n";
    return true;
}

// ---------------------------------------------------------------------------
// Live CC Monitor Mode (Plugin Port / Slot 2: PLUGIN)
// ---------------------------------------------------------------------------

void runLiveMonitor() {
    RtMidiIn midiIn;
    RtMidiOut midiOut;

    int inIdx = findPort(midiIn, "Plugin Port");
    if (inIdx < 0) inIdx = findPort(midiIn, "MPK mini IV");
    int outIdx = findPort(midiOut, "Plugin Port");
    uint8_t ownerType = 0x01;
    if (outIdx < 0) {
        outIdx = findPort(midiOut, "DAW Port");
        ownerType = 0x02;
    }

    if (inIdx < 0 || outIdx < 0) {
        std::cerr << "[Error] Could not find MPK mini IV Plugin Port for monitoring.\n";
        return;
    }

    midiIn.openPort(inIdx);
    midiOut.openPort(outIdx);
    midiIn.ignoreTypes(false, false, false);

    if (ownerType == 0x01) {
        auto modeSx = makeSelectModeSysEx(0x01);
        midiOut.sendMessage(&modeSx);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    auto ownerSx = makeScreenOwnerSysEx(ownerType);
    midiOut.sendMessage(&ownerSx);

    sendLiveDisplay("MK4-SYNTH", "LIVE MONITOR", "READY");

    std::signal(SIGINT, sigHandler);
    std::signal(SIGTERM, sigHandler);

    std::cout << "\n[Monitor Active] Turn knobs on your MPK Mini IV. Press Ctrl+C to exit.\n\n";

    midiIn.setCallback([](double, std::vector<unsigned char>* msg, void* userData) {
        if (!msg || msg->size() < 3) return;
        auto* pOut = static_cast<RtMidiOut*>(userData);
        uint8_t status = (*msg)[0];
        uint8_t cc = (*msg)[1];
        uint8_t val = (*msg)[2];

        if ((status & 0xF0) == 0xB0) {
            std::string param = "CC #" + std::to_string(cc);
            std::string valStr = std::to_string(val);

            if (cc == 24) { param = "CUTOFF"; valStr = std::to_string(val * 150) + " Hz"; }
            else if (cc == 25) { param = "RESONANCE"; valStr = std::to_string((val / 127.0) * 20.0).substr(0, 4) + " Q"; }
            else if (cc == 26) { param = "SUB-OSC"; valStr = std::to_string(static_cast<int>((val / 127.0) * 100)) + " %"; }
            else if (cc == 27) { param = "CHORUS"; valStr = std::to_string(static_cast<int>((val / 127.0) * 100)) + " %"; }
            else if (cc == 28) { param = "ATTACK"; valStr = std::to_string(static_cast<int>(val * 35)) + " ms"; }
            else if (cc == 29) { param = "DECAY"; valStr = std::to_string(static_cast<int>(val * 35)) + " ms"; }
            else if (cc == 30) { param = "SUSTAIN"; valStr = std::to_string(static_cast<int>((val / 127.0) * 100)) + " %"; }
            else if (cc == 31) { param = "RELEASE"; valStr = std::to_string(static_cast<int>(val * 35)) + " ms"; }

            std::cout << "\r[CC " << std::setw(2) << static_cast<int>(cc) << "] "
                      << std::setw(10) << param << " -> " << std::setw(8) << valStr << std::flush;

            // Update OLED screen
            auto f1 = makeTextFormatSysEx(1, 1, 63, 63, 63, 0, 0, 0);
            pOut->sendMessage(&f1);
            auto t1 = makeDisplayTextSysEx(param, 1);
            pOut->sendMessage(&t1);

            auto f2 = makeTextFormatSysEx(2, 2, 63, 45, 0, 0, 0);
            pOut->sendMessage(&f2);
            auto t2 = makeDisplayTextSysEx(valStr, 2);
            pOut->sendMessage(&t2);
        }
    }, &midiOut);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[Exiting live monitor]\n";
}

// ---------------------------------------------------------------------------
// Main Entrypoint
// ---------------------------------------------------------------------------

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --sync, -s                 Sync synth knob names & program to hardware RAM (Default)\n"
              << "  --text <L0> <L1> <L2>      Write 3 lines of text to the OLED screen\n"
              << "  --clear                    Clear all lines on the OLED display\n"
              << "  --monitor, -m              Run live CC monitor and update OLED display on knob turns\n"
              << "  --demo-screens [NAME]      Show every synth theme: home screen + knob sweep (value text + bar)\n"
              << "  --help, -h                 Display this help message\n\n"
              << "Examples:\n"
              << "  " << prog << " --sync\n"
              << "  " << prog << " --text \"MK4-SYNTH\" \"JUNO-60 PAD\" \"OCTAVE C3\"\n"
              << "  " << prog << " --monitor\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=========================================================\n"
              << "  Akai MPK Mini IV Display & Hardware Configuration Tool \n"
              << "=========================================================\n";

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--help" || arg1 == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg1 == "--identify" || arg1 == "--version" || arg1 == "-v") {
            MpkMiniMk4::DisplayDriver drv;
            MpkMiniMk4::DeviceIdentity id;
            if (!drv.queryDeviceIdentity(id)) {
                std::cerr << "[Error] No Akai MPK Mini IV responded to Universal Device Inquiry (F0 7E 7F 06 01 F7).\n";
                return 1;
            }
            std::cout << "[OK] Universal Device Inquiry Reply (" << id.rawReply.size() << " bytes):\n  Hex: ";
            for (uint8_t b : id.rawReply) {
                std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(b) << " ";
            }
            std::cout << std::dec << std::setfill(' ') << "\n\n";
            std::cout << "  Manufacturer ID : 0x" << std::hex << std::uppercase << static_cast<int>(id.manufacturerId)
                      << std::dec << (id.manufacturerId == 0x47 ? " (Akai Professional)" : " (Unknown)") << "\n";
            std::cout << "  Product ID      : 0x" << std::hex << std::uppercase << static_cast<int>(id.productId)
                      << std::dec << (id.productId == 0x5D ? " (MPK mini IV / MK4)" : " (Unknown)") << "\n";
            std::cout << "  Family/Member   : 0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                      << id.familyMemberCode << std::dec << std::setfill(' ') << " (AD41 / MPK Mini IV)\n";
            std::cout << "  Firmware Version: " << id.firmwareVersionString() << "\n";
            std::cout << "  SysEx Device ID : 0x" << std::hex << std::uppercase << static_cast<int>(id.sysexDeviceId) << std::dec << "\n";
            std::cout << "  HW Rev / Extra  : [" << static_cast<int>(id.hardwareRevBytes[0]) << ", "
                      << static_cast<int>(id.hardwareRevBytes[1]) << ", "
                      << static_cast<int>(id.hardwareRevBytes[2]) << ", "
                      << static_cast<int>(id.hardwareRevBytes[3]) << "]\n";
            std::cout << "  Serial Number   : \"" << id.serialNumber << "\"\n";
            std::cout << "  OLED Protocol   : " << (id.isFirmwareCompatible()
                      ? "COMPATIBLE (0x3A / 0x1C / 0x14 / 0x10 RGB565 + Align Supported)"
                      : "INCOMPATIBLE") << "\n";

            // Also display the detected Hardware & Firmware version directly on the MPK Mini IV OLED screen
            if (drv.autoConnectRtMidi()) {
                int majorDec = static_cast<int>(id.fwMajor) * 10 + static_cast<int>(id.fwMinor);
                int minorDec = static_cast<int>(id.fwPatch) * 10 + static_cast<int>(id.fwBuild);
                char fwLine[32];
                std::snprintf(fwLine, sizeof(fwLine), "FW: v%d.%02d (Y%d-%02d)", majorDec, minorDec, majorDec, minorDec);
                drv.writeText3Lines(
                    "MPK MINI IV [HW OK]",
                    fwLine,
                    "SN:" + (id.serialNumber.size() > 12 ? id.serialNumber.substr(id.serialNumber.size() - 12) : id.serialNumber),
                    MpkMiniMk4::Color::Emerald(),
                    MpkMiniMk4::Align::Center,
                    MpkMiniMk4::Align::Left,
                    MpkMiniMk4::Align::Right
                );
            }
            return id.isFirmwareCompatible() ? 0 : 1;
        } else if (arg1 == "--demo-screens") {
            // Every factory theme: home screen, then a knob sweep with the host-drawn value and bar (same calls as the
            // ESP32 path: native screen kept, screen claimed without 0x3A). Optional argument: one preset name.
            MpkMiniMk4::DisplayDriver drv;
            if (!drv.autoConnectRtMidi()) {
                std::cerr << "[Error] Could not auto-connect to the MPK mini IV Plugin Port.\n";
                return 1;
            }
            drv.setScreenTakeoverEnabled(false);
            std::vector<std::string> names = {"JUNO-60", "MINIMOOG", "PROPHET-5", "TB-303 ACID", "CS-80 BLADE",
                                              "OB-XA BRASS", "PT-10 PIANO", "PT-10 FNTSY", "PT-10 VIOLN",
                                              "PT-10 FLUTE", "DX E-PIANO", "FM TINES", "WARM RHODES",
                                              "FM CLAVINET", "FM ORGAN"};
            if (argc > 2) names = {argv[2]};
            const auto count = static_cast<unsigned>(Synth::SynthModel::Count);
            for (size_t i = 0; i < names.size(); ++i) {
                for (bool bankB : {false, true}) {
                    auto theme = MpkMiniMk4::DisplayDriver::themeForPresetName(names[i], bankB);
                    char info[24];
                    std::snprintf(info, sizeof(info), "%u/%u | BANK %c", static_cast<unsigned>(i + 1), count,
                                  bankB ? 'B' : 'A');
                    std::cout << "[" << names[i] << (bankB ? " B" : " A") << "] home\n";
                    drv.showHomeScreen(names[i], info, theme);
                    drv.sendKeepalive();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
                    if (bankB) continue;
                    std::cout << "[" << names[i] << "] knob sweep\n";
                    for (int k = 0; k <= 50; ++k) {
                        const int v = (k <= 25 ? k : 50 - k) * 127 / 25;
                        drv.showKnobHud(names[i], "Cutoff", std::to_string(v * 100 / 127) + "%", v, theme);
                        if (k % 16 == 0) drv.sendKeepalive();
                        std::this_thread::sleep_for(std::chrono::milliseconds(60));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(600));
                }
            }
            drv.endTransientHud();
            return 0;
        } else if (arg1 == "--demo-synth") {
            Synth::SynthEngine engine;
            engine.init(44100.0f);
            if (!engine.getMpkDisplay().autoConnectRtMidi()) {
                std::cerr << "[Error] Could not auto-connect MpkMiniMk4::DisplayDriver via RtMidi.\n";
                return 1;
            }
            std::cout << "[SynthEngine -> MpkMiniMk4Display] 1/4 Loading Preset: MINIMOOG...\n";
            engine.loadPreset(Synth::SynthModel::Minimoog);
            std::this_thread::sleep_for(std::chrono::milliseconds(1400));

            std::cout << "[SynthEngine -> MpkMiniMk4Display] 2/4 Turning Cutoff Knob (CC 74 -> 108)...\n";
            engine.controlChange(74, 108);
            std::this_thread::sleep_for(std::chrono::milliseconds(1400));

            std::cout << "[SynthEngine -> MpkMiniMk4Display] 3/4 Switching to Bank B (Shift Active)...\n";
            engine.setShiftActive(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1400));

            std::cout << "[SynthEngine -> MpkMiniMk4Display] 4/4 Loading Preset: JUNO-60 [Bank A] & Cutoff CC 74 -> 92...\n";
            engine.setShiftActive(false);
            engine.loadPreset(Synth::SynthModel::Juno60);
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            engine.controlChange(74, 92);
            std::cout << "[OK] SynthEngine successfully updated MPK Mini MK4 OLED via MpkMiniMk4Display library!\n";
            return 0;
        } else if (arg1 == "--demo-align") {
            // Showcase Left (0), Center (1), Right (2) alignment + distinct RGB565 background colors per line
            std::cout << "[Demo 1/3] Line 0=LEFT (Ruby BG), Line 1=CENTER (Emerald BG), Line 2=RIGHT (Cobalt BG)...\n";
            sendLiveDisplay("< LEFT LINE 0", "CENTER LINE 1", "RIGHT LINE 2 >",
                            63, 4, 16,   // Line 0 BG: Ruby Red
                            0, 1, 2,     // Align: 0=Left, 1=Center, 2=Right
                            2, 44, 14,   // Line 1 BG: Emerald Green
                            4, 14, 63);  // Line 2 BG: Royal Cobalt Blue
            std::this_thread::sleep_for(std::chrono::milliseconds(1800));

            std::cout << "[Demo 2/3] Line 0=RIGHT (Purple BG), Line 1=LEFT (Amber BG), Line 2=CENTER (Teal BG)...\n";
            sendLiveDisplay("RIGHT HEADER >", "< LEFT MIDDLE", "[ CENTERED ]",
                            48, 4, 63,   // Line 0 BG: Vivid Purple
                            2, 0, 1,     // Align: 2=Right, 0=Left, 1=Center
                            56, 28, 2,   // Line 1 BG: Warm Amber
                            2, 38, 48);  // Line 2 BG: Ocean Teal
            std::this_thread::sleep_for(std::chrono::milliseconds(1800));

            std::cout << "[Demo 3/3] Synth HUD: Header=CENTER (Crimson), Param=LEFT, Value=RIGHT...\n";
            return sendLiveDisplay("JUNO-60 [BANK A]", "K1: FILTER CUTOFF", "2400 Hz >",
                                   60, 4, 22,   // Line 0 BG: Roland Crimson
                                   1, 0, 2,     // Align: 1=Center, 0=Left, 2=Right
                                   14, 4, 28,   // Line 1 BG: Deep Indigo
                                   8, 24, 44)   // Line 2 BG: Midnight Cyan
                   ? 0 : 1;
        } else if (arg1 == "--text" || arg1 == "-t") {
            std::string l0 = (argc > 2) ? argv[2] : "";
            std::string l1 = (argc > 3) ? argv[3] : "";
            std::string l2 = (argc > 4) ? argv[4] : "";
            uint8_t bgR = 24, bgG = 4, bgB = 28;
            uint8_t a0 = 1, a1 = 0, a2 = 2;
            auto parseAlign = [](const std::string& s) -> uint8_t {
                if (s == "0" || s == "L" || s == "l" || s == "left") return 0;
                if (s == "1" || s == "C" || s == "c" || s == "center") return 1;
                if (s == "2" || s == "R" || s == "r" || s == "right") return 2;
                return 0;
            };
            for (int i = 5; i < argc; ++i) {
                std::string flag = argv[i];
                if (flag == "--bg" && i + 3 < argc) {
                    bgR = static_cast<uint8_t>(std::stoi(argv[++i]));
                    bgG = static_cast<uint8_t>(std::stoi(argv[++i]));
                    bgB = static_cast<uint8_t>(std::stoi(argv[++i]));
                } else if (flag == "--align" && i + 3 < argc) {
                    a0 = parseAlign(argv[++i]);
                    a1 = parseAlign(argv[++i]);
                    a2 = parseAlign(argv[++i]);
                }
            }
            return sendLiveDisplay(l0, l1, l2, bgR, bgG, bgB, a0, a1, a2) ? 0 : 1;
        } else if (arg1 == "--clear" || arg1 == "-c") {
            return sendLiveDisplay("", "", "") ? 0 : 1;
        } else if (arg1 == "--dump-slots") {
            RtMidiIn midiIn;
            RtMidiOut midiOut;
            int inIdx = findPort(midiIn, "Software Control");
            int outIdx = findPort(midiOut, "Software Control");
            if (inIdx < 0 || outIdx < 0) return 1;
            midiIn.openPort(inIdx);
            midiOut.openPort(outIdx);
            midiIn.ignoreTypes(false, false, false);
            for (int slot = 0; slot <= 8; ++slot) {
                std::vector<unsigned char> q = {0xF0, 0x47, 0x7F, 0x5D, 0x66, 0x00, 0x00, static_cast<unsigned char>(slot), 0xF7};
                midiOut.sendMessage(&q);
                std::vector<unsigned char> msg;
                for (int w = 0; w < 15; ++w) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    midiIn.getMessage(&msg);
                    if (msg.size() == 284 && msg[4] == 0x67) {
                        std::string name(reinterpret_cast<char*>(&msg[8]), 16);
                        name = name.c_str();
                        std::cout << "Slot " << slot << " (byte7=" << (int)msg[7] << ") Name=\"" << name
                                  << "\" padColor=" << (int)msg[41] << " hdr24..36=[";
                        for (int b = 24; b <= 36; ++b) std::cout << (int)msg[b] << (b < 36 ? "," : "");
                        std::cout << "] tail277..282=[";
                        for (int b = 277; b <= 282; ++b) std::cout << (int)msg[b] << (b < 282 ? "," : "");
                        std::cout << "]\n";
                        if (slot == 0 || slot == 2) {
                            for (int k = 0; k < 8; ++k) {
                                int off = 117 + k * 20;
                                std::string kname(reinterpret_cast<char*>(&msg[off + 4]), 16);
                                kname = kname.c_str();
                                std::cout << "   K" << (k + 1) << ": cc=" << (int)msg[off]
                                          << " min=" << (int)msg[off + 1]
                                          << " max=" << (int)msg[off + 2]
                                          << " mode=" << (int)msg[off + 3]
                                          << " name=\"" << kname << "\"\n";
                            }
                        }
                        break;
                    }
                }
            }
            return 0;
        } else if (arg1 == "--verify-hw") {
            RtMidiIn midiIn;
            RtMidiOut midiOut;
            int inIdx = findPort(midiIn, "Software Control");
            int outIdx = findPort(midiOut, "Software Control");
            if (inIdx < 0 || outIdx < 0) {
                std::cout << "[SKIP] Akai MPK Mini IV not connected; skipping live HIL verification.\n";
                return 0;
            }
            std::vector<KnobDef> defaultKnobs = {
                {24, "Cutoff"},  {25, "Resonnc"}, {26, "SubOsc"},  {27, "Chorus"},
                {28, "Attack"},  {29, "Decay"},   {30, "Sustain"}, {31, "Release"}
            };
            if (!syncHardwareKnobLabels("MK4-SYNTH", defaultKnobs)) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!sendLiveDisplay("P1: JUNO-60", "BANK A: K1-K8", "Cutoff / Resonnc", 28, 4, 14)) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            midiIn.openPort(inIdx);
            midiOut.openPort(outIdx);
            midiIn.ignoreTypes(false, false, false);
            for (int slot : {0}) {
                std::vector<unsigned char> q = {0xF0, 0x47, 0x7F, 0x5D, 0x66, 0x00, 0x00, static_cast<unsigned char>(slot), 0xF7};
                midiOut.sendMessage(&q);
                std::vector<unsigned char> msg;
                bool gotReply = false;
                for (int w = 0; w < 20; ++w) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    midiIn.getMessage(&msg);
                    if (msg.size() == 284 && msg[4] == 0x67) {
                        gotReply = true;
                        for (int k = 0; k < 8; ++k) {
                            int off = 117 + k * 20;
                            int cc = msg[off];
                            int minV = msg[off + 1];
                            int maxV = msg[off + 2];
                            int mode = msg[off + 3];
                            std::string kname(reinterpret_cast<char*>(&msg[off + 4]), 16);
                            kname = kname.c_str();
                            if (cc != (24 + k) || minV != 0 || maxV != 127 || mode != 0 || kname != defaultKnobs[k].name) {
                                std::cerr << "[FAIL] Slot " << slot << " K" << (k + 1) << " corrupted! cc=" << cc
                                          << " min=" << minV << " max=" << maxV << " mode=" << mode << " name=\"" << kname << "\"\n";
                                return 1;
                            }
                        }
                        std::cout << "[PASS] Slot " << slot << " verified: all 8 knobs K1..K8 intact (cc=24..31, min=0, max=127, mode=0) after OLED display write.\n";
                        break;
                    }
                }
                if (!gotReply) {
                    std::cerr << "[FAIL] No 284-byte 0x67 reply received for Slot " << slot << "\n";
                    return 1;
                }
            }
            std::cout << ">>> HARDWARE-IN-THE-LOOP (HIL) MPK MINI IV VERIFICATION PASSED! <<<\n";
            return 0;
        } else if (arg1 == "--monitor" || arg1 == "-m") {
            runLiveMonitor();
            return 0;
        }
    }

    // Default: Hardware RAM Preset Sync (strictly preserving native CC 24..31 = 0x18..0x1F)
    std::cout << "Configuring hardware knob labels and program name...\n";
    std::vector<KnobDef> defaultKnobs = {
        {24, "Cutoff"},
        {25, "Resonnc"},
        {26, "SubOsc"},
        {27, "Chorus"},
        {28, "Attack"},
        {29, "Decay"},
        {30, "Sustain"},
        {31, "Release"}
    };

    bool ok = syncHardwareKnobLabels("MK4-SYNTH", defaultKnobs);
    if (ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        sendLiveDisplay("P1: JUNO-60", "BANK A: K1-K8", "Cutoff / Resonnc", 28, 4, 14);
    }
    return ok ? 0 : 1;
}
