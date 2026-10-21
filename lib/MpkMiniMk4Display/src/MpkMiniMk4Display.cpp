#include "MpkMiniMk4Display.h"
#include "MpkOledFont.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

namespace MpkMiniMk4 {

namespace {

// Clean 284-byte MPK Mini IV Slot 2 (PLUGIN) preset template extracted from physical hardware
static constexpr uint8_t DEFAULT_RAM_PRESET_TEMPLATE[284] = {
    240,71,127,93,100,2,20,0,66,79,76,68,45,67,65,82,83,79,78,0,0,0,0,0,
    0,9,0,120,3,2,0,0,1,0,127,0,1,36,32,0,12,41,37,33,1,12,41,38,34,2,12,
    41,39,35,3,12,41,40,36,4,12,41,41,37,5,12,41,42,38,6,12,41,43,39,7,12,
    41,44,40,8,12,41,45,41,9,12,41,46,42,10,12,41,47,43,11,12,41,48,44,12,
    12,41,49,45,13,12,41,50,46,14,12,41,51,47,15,12,41,
    24,0,127,0,67,117,116,111,102,102,0,0,0,0,0,0,0,0,0,0,
    25,0,127,0,82,101,115,111,110,110,99,0,0,0,0,0,0,0,0,0,
    26,0,127,0,83,117,98,79,115,99,0,0,0,0,0,0,0,0,0,0,
    27,0,127,0,67,104,111,114,117,115,0,0,0,0,0,0,0,0,0,0,
    28,0,127,0,65,116,116,97,99,107,0,0,0,0,0,0,0,0,0,0,
    29,0,127,0,68,101,99,97,121,0,0,0,0,0,0,0,0,0,0,0,
    30,0,127,0,83,117,115,116,97,105,110,0,0,0,0,0,0,0,0,0,
    31,0,127,0,82,101,108,101,97,115,101,0,0,0,0,0,0,0,0,0,
    1,1,2,1,1,1,247
};

static std::string toUpperAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

std::string DeviceIdentity::firmwareVersionString() const {
    if (!valid) return "Unknown";
    // Bytes 9..12 are 4 decimal digits [d0, d1, d2, d3] -> v<d0><d1>.<d2><d3> (e.g. [0,1,4,1] -> v1.41 / Y1-41)
    int majorDec = static_cast<int>(fwMajor) * 10 + static_cast<int>(fwMinor);
    int minorDec = static_cast<int>(fwPatch) * 10 + static_cast<int>(fwBuild);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "v%d.%02d (Y%d-%02d / raw %u.%u.%u.%u)",
                  majorDec, minorDec, majorDec, minorDec,
                  fwMajor, fwMinor, fwPatch, fwBuild);
    return std::string(buf);
}

std::string DeviceIdentity::hardwareVersionString() const {
    if (!valid) return "Unknown";
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "Mfg=0x%02X (Akai) Product=0x%02X (MPK mini IV) Family=0x%04X Rev=[%u.%u.%u.%u]",
                  manufacturerId, productId, familyMemberCode,
                  hardwareRevBytes[0], hardwareRevBytes[1], hardwareRevBytes[2], hardwareRevBytes[3]);
    return std::string(buf);
}

DisplayDriver::DisplayDriver(uint8_t deviceId)
    : deviceId_(deviceId),
      baseRamTemplate_(std::begin(DEFAULT_RAM_PRESET_TEMPLATE), std::end(DEFAULT_RAM_PRESET_TEMPLATE)) {}

DeviceIdentity DisplayDriver::parseDeviceIdentityReply(const uint8_t* data, size_t len) {
    DeviceIdentity id{};
    if (!data || len < 15) return id;
    if (data[0] != 0xF0 || data[1] != 0x7E || data[3] != 0x06 || data[4] != 0x02) {
        return id;
    }

    id.rawReply.assign(data, data + len);
    id.deviceId         = data[2];
    id.manufacturerId   = data[5];
    id.productId        = data[6];
    id.familyMemberCode = static_cast<uint16_t>(data[7]) | (static_cast<uint16_t>(data[8]) << 8);
    id.fwMajor          = data[9];
    id.fwMinor          = data[10];
    id.fwPatch          = data[11];
    id.fwBuild          = data[12];
    id.sysexDeviceId    = (len > 13) ? data[13] : data[2];

    for (size_t i = 0; i < 4 && (14 + i) < len - 1; ++i) {
        id.hardwareRevBytes[i] = data[14 + i];
    }

    if (len >= 35) {
        std::string sn;
        for (size_t i = 18; i < 34; ++i) {
            char c = static_cast<char>(data[i] & 0x7F);
            if (c >= 32 && c < 127) {
                sn.push_back(c);
            }
        }
        // Trim trailing whitespace
        while (!sn.empty() && (sn.back() == ' ' || sn.back() == '\0')) {
            sn.pop_back();
        }
        id.serialNumber = sn;
    }

    id.valid = true;
    return id;
}

void DisplayDriver::sendToPluginPort(const std::vector<uint8_t>& packet) {
    if (pluginPortSender_ && !packet.empty()) {
        pluginPortSender_(packet.data(), packet.size());
    }
}

void DisplayDriver::sendToSwControlPort(const std::vector<uint8_t>& packet) {
    if (swControlPortSender_ && !packet.empty()) {
        swControlPortSender_(packet.data(), packet.size());
    }
}

#if defined(MPK_DISPLAY_WITH_RTMIDI)
bool DisplayDriver::queryDeviceIdentity(DeviceIdentity& outIdentity) {
    outIdentity = DeviceIdentity{};
    try {
        RtMidiIn midiIn;
        RtMidiOut midiOut;
        const char* portCandidates[] = {"Plugin Port", "Software Control", "MIDI Port", "DAW Port"};
        for (const char* pat : portCandidates) {
            int inIdx  = -1;
            int outIdx = -1;
            for (unsigned int i = 0; i < midiIn.getPortCount(); ++i) {
                if (midiIn.getPortName(i).find(pat) != std::string::npos) {
                    inIdx = static_cast<int>(i);
                    break;
                }
            }
            for (unsigned int i = 0; i < midiOut.getPortCount(); ++i) {
                if (midiOut.getPortName(i).find(pat) != std::string::npos) {
                    outIdx = static_cast<int>(i);
                    break;
                }
            }
            if (inIdx < 0 || outIdx < 0) continue;

            RtMidiIn pIn;
            RtMidiOut pOut;
            pIn.openPort(inIdx);
            pOut.openPort(outIdx);
            pIn.ignoreTypes(false, false, false);

            // Drain any stale incoming messages
            std::vector<unsigned char> msg;
            while (true) {
                pIn.getMessage(&msg);
                if (msg.empty()) break;
            }

            // Send Universal Device Inquiry: F0 7E 7F 06 01 F7
            auto inquiry = makeDeviceInquirySysEx();
            pOut.sendMessage(&inquiry);

            for (int attempt = 0; attempt < 15; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                pIn.getMessage(&msg);
                if (msg.size() >= 15 && msg[0] == 0xF0 && msg[1] == 0x7E && msg[3] == 0x06 && msg[4] == 0x02) {
                    outIdentity = parseDeviceIdentityReply(msg.data(), msg.size());
                    if (outIdentity.valid) {
                        deviceIdentity_ = outIdentity;
                        deviceId_       = outIdentity.deviceId;
                        return true;
                    }
                }
            }
        }
    } catch (...) {}
    return false;
}

bool DisplayDriver::autoConnectRtMidi() {
    try {
        // First query hardware & firmware identity via Universal Device Inquiry (06 01 -> 06 02)
        queryDeviceIdentity(deviceIdentity_);

        RtMidiOut probe;
        int pluginIdx = -1;
        int dawIdx    = -1;
        int swIdx     = -1;

        for (unsigned int i = 0; i < probe.getPortCount(); ++i) {
            std::string name = probe.getPortName(i);
            if (pluginIdx < 0 && name.find("Plugin Port") != std::string::npos) {
                pluginIdx = static_cast<int>(i);
            } else if (dawIdx < 0 && name.find("DAW Port") != std::string::npos) {
                dawIdx = static_cast<int>(i);
            } else if (swIdx < 0 && name.find("Software Control") != std::string::npos) {
                swIdx = static_cast<int>(i);
            }
        }

        // Query live hardware template on Software Control Port if available
        if (swIdx >= 0) {
            try {
                RtMidiIn swIn;
                RtMidiOut swOut;
                int swInIdx = -1;
                for (unsigned int i = 0; i < swIn.getPortCount(); ++i) {
                    if (swIn.getPortName(i).find("Software Control") != std::string::npos) {
                        swInIdx = static_cast<int>(i);
                        break;
                    }
                }
                if (swInIdx >= 0) {
                    swIn.openPort(swInIdx);
                    swOut.openPort(swIdx);
                    swIn.ignoreTypes(false, false, false);
                    std::vector<unsigned char> query = {0xF0, 0x47, 0x7F, 0x5D, 0x66, 0x00, 0x00, 0x02, 0xF7};
                    swOut.sendMessage(&query);
                    std::vector<unsigned char> msg;
                    for (int attempt = 0; attempt < 15; ++attempt) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(15));
                        swIn.getMessage(&msg);
                        if (msg.size() == RAM_PRESET_SIZE && msg[4] == 0x67) {
                            baseRamTemplate_ = msg;
                            break;
                        }
                    }
                }
            } catch (...) {}

            rtSwControlOut_ = std::make_unique<RtMidiOut>();
            rtSwControlOut_->openPort(swIdx);
            swControlPortSender_ = [this](const uint8_t* data, size_t len) {
                if (!rtSwControlOut_) return;
                std::vector<unsigned char> msg(data, data + len);
                rtSwControlOut_->sendMessage(&msg);
            };
        }

        // Prefer Plugin Port exclusively (the MK4 keeps a single active screen owner for all ports)
        int targetPortIdx = (pluginIdx >= 0) ? pluginIdx : dawIdx;
        if (targetPortIdx < 0) {
            return false;
        }

        rtIsPluginPort_ = (pluginIdx >= 0);
        rtOwnerType_    = rtIsPluginPort_ ? static_cast<uint8_t>(OwnerType::PluginSVK0)
                                          : static_cast<uint8_t>(OwnerType::Daw);

        rtPluginOut_ = std::make_unique<RtMidiOut>();
        rtPluginOut_->openPort(targetPortIdx);
        pluginPortSender_ = [this](const uint8_t* data, size_t len) {
            if (!rtPluginOut_) return;
            std::vector<unsigned char> msg(data, data + len);
            rtPluginOut_->sendMessage(&msg);
        };

        return true;
    } catch (...) {
        return false;
    }
}
#endif

// -----------------------------------------------------------------------------
// Static SysEx Packet Builders
// -----------------------------------------------------------------------------

std::vector<uint8_t> DisplayDriver::makeDeviceInquirySysEx() {
    return {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
}

std::vector<uint8_t> DisplayDriver::makeParamQuerySysEx(uint16_t paramIdx, uint8_t devId) {
    return {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x6A, 0x00, 0x02,
            static_cast<uint8_t>((paramIdx >> 7) & 0x7F), static_cast<uint8_t>(paramIdx & 0x7F), 0xF7};
}

std::vector<uint8_t> DisplayDriver::makeQueryOwnerSysEx(uint8_t devId) {
    return {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x2D, 0x00, 0x00, 0xF7};
}

std::vector<uint8_t> DisplayDriver::makeSelectModeSysEx(HardwareMode mode, uint8_t devId) {
    return {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x3A, 0x00, 0x01,
            static_cast<uint8_t>(mode), 0xF7};
}

std::vector<uint8_t> DisplayDriver::makeScreenOwnerSysEx(OwnerType owner, const std::string& uuid16, uint8_t devId) {
    std::string id = uuid16;
    id.resize(16, ' ');
    std::vector<uint8_t> sx = {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV,
                               0x1C, 0x00, 0x11, static_cast<uint8_t>(owner)};
    for (char c : id) {
        sx.push_back(static_cast<uint8_t>(c) & 0x7F);
    }
    sx.push_back(0xF7);
    return sx;
}

std::vector<uint8_t> DisplayDriver::makeHeartbeatReplySysEx(OwnerType owner, const std::string& uuid16, uint8_t devId) {
    std::string id = uuid16;
    id.resize(16, ' ');
    std::vector<uint8_t> sx = {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV,
                               0x19, 0x00, 0x11, static_cast<uint8_t>(owner)};
    for (char c : id) {
        sx.push_back(static_cast<uint8_t>(c) & 0x7F);
    }
    sx.push_back(0xF7);
    return sx;
}

std::vector<uint8_t> DisplayDriver::makeLineFormatSysEx(uint8_t line, Align align,
                                                        Color fg, Color bg,
                                                        Layout layout, uint8_t devId) {
    std::vector<uint8_t> payload = {
        static_cast<uint8_t>(static_cast<uint8_t>(layout) & 0x03),
        static_cast<uint8_t>(line & 0x07),
        static_cast<uint8_t>(static_cast<uint8_t>(align) & 0x03),
        fg.r5(), fg.g6(), fg.b5(),
        bg.r5(), bg.g6(), bg.b5()
    };
    uint8_t msb = static_cast<uint8_t>(payload.size() / 128);
    uint8_t lsb = static_cast<uint8_t>(payload.size() % 128);
    std::vector<uint8_t> sx = {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x14, msb, lsb};
    sx.insert(sx.end(), payload.begin(), payload.end());
    sx.push_back(0xF7);
    return sx;
}

std::vector<uint8_t> DisplayDriver::makePaletteLineColorSysEx(uint8_t line, uint8_t paletteIdx, uint8_t devId) {
    return {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x11, 0x00, 0x02,
            static_cast<uint8_t>(line & 0x07), static_cast<uint8_t>(paletteIdx & 0x0F), 0xF7};
}

std::vector<uint8_t> DisplayDriver::makeLineTextSysEx(uint8_t line, const std::string& text,
                                                      Layout layout, uint8_t devId) {
    std::string clean = text.substr(0, 31); // Firmware clamps at 31 chars + null terminator
    std::vector<uint8_t> payload = {
        static_cast<uint8_t>(static_cast<uint8_t>(layout) & 0x03),
        static_cast<uint8_t>(line & 0x07)
    };
    for (char c : clean) {
        payload.push_back(static_cast<uint8_t>(c) & 0x7F);
    }
    payload.push_back(0x00); // Null terminator
    uint8_t msb = static_cast<uint8_t>(payload.size() / 128);
    uint8_t lsb = static_cast<uint8_t>(payload.size() % 128);
    std::vector<uint8_t> sx = {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x10, msb, lsb};
    sx.insert(sx.end(), payload.begin(), payload.end());
    sx.push_back(0xF7);
    return sx;
}

std::vector<uint8_t> DisplayDriver::makeBlitSysEx(int x, int y, int w, int h, const uint16_t* pixels, uint8_t devId) {
    const size_t count   = (w > 0 && h > 0) ? static_cast<size_t>(w) * static_cast<size_t>(h) : 0;
    const size_t payload = 8 + count * 3;
    std::vector<uint8_t> sx;
    sx.reserve(payload + 8);
    sx = {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x71,
          static_cast<uint8_t>((payload >> 7) & 0x7F), static_cast<uint8_t>(payload & 0x7F)};
    for (int v : {x, y, w, h}) {
        sx.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
        sx.push_back(static_cast<uint8_t>(v & 0x7F));
    }
    for (size_t i = 0; i < count; ++i) {
        sx.push_back(static_cast<uint8_t>(pixels[i] >> 11));
        sx.push_back(static_cast<uint8_t>((pixels[i] >> 5) & 0x3F));
        sx.push_back(static_cast<uint8_t>(pixels[i] & 0x1F));
    }
    sx.push_back(0xF7);
    return sx;
}

std::vector<uint8_t> DisplayDriver::makeKnobValueSysEx(uint8_t knobIndex, uint8_t value7Bit, uint8_t devId) {
    uint8_t k   = (knobIndex > 7) ? 7 : knobIndex;
    uint8_t val = (value7Bit > 127) ? 127 : value7Bit;
    return {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x12, 0x00, 0x02, k, val, 0xF7};
}

std::vector<uint8_t> DisplayDriver::makeSetTempoSysEx(uint16_t bpm, uint8_t devId) {
    if (bpm < 30) bpm = 30;
    if (bpm > 240) bpm = 240;
    return {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x6C, 0x00, 0x02,
            static_cast<uint8_t>(bpm >> 7), static_cast<uint8_t>(bpm & 0x7F), 0xF7};
}

std::vector<uint8_t> DisplayDriver::makeBankSelectSysEx(bool isBankB, uint8_t devId) {
    // The MK4 accepts this ONLY on USB-MIDI virtual cable 2 (
    // silently dropped on cable 1 -- transports must route this packet to cable 2):
    // SysEx 0x3A sets hardware Bank state & BANK A/B button LED:
    // 0x01 = Bank A (White LED)
    // 0x00 = Bank B (Red LED)
    return {0xF0, MANUFACTURER_AKAI, devId, MODEL_MPK_MINI_IV, 0x3A, 0x00, 0x01,
            static_cast<uint8_t>(isBankB ? 0x00 : 0x01), 0xF7};
}

std::vector<uint8_t> DisplayDriver::makePadColorSysEx(uint8_t padIndex, uint8_t colorIndex, uint8_t devId) {
    return makeKnobValueSysEx(padIndex, colorIndex, devId);
}


std::vector<uint8_t> DisplayDriver::makeRamPresetSysEx(const std::string& programName,
                                                       const std::array<std::string, 8>& knobLabels,
                                                       uint8_t padColorIndex,
                                                       const std::vector<uint8_t>* baseTemplate) {
    std::vector<uint8_t> packet = (baseTemplate && baseTemplate->size() == RAM_PRESET_SIZE)
        ? *baseTemplate
        : std::vector<uint8_t>(std::begin(DEFAULT_RAM_PRESET_TEMPLATE), std::end(DEFAULT_RAM_PRESET_TEMPLATE));

    packet[0] = 0xF0;
    packet[1] = MANUFACTURER_AKAI;
    packet[2] = 0x7F;
    packet[3] = MODEL_MPK_MINI_IV;
    packet[4] = 0x64; // Write preset command (0x64)
    packet[7] = 0x00; // Slot 0 Active Hardware RAM (zero SPI flash wear)

    // Program Name (offset 8..23, 16 bytes)
    for (size_t i = 0; i < 16; ++i) {
        packet[8 + i] = (i < programName.size()) ? (static_cast<uint8_t>(programName[i]) & 0x7F) : 0x00;
    }

    // 16 RGB Pad Colors (offset 37 + p * 5 + 4)
    for (size_t p = 0; p < 16; ++p) {
        size_t padOff = 37 + p * 5 + 4;
        if (padOff < packet.size()) {
            packet[padOff] = static_cast<uint8_t>(padColorIndex & 0x7F);
        }
    }

    // 8 Knob Descriptors (offset 117 + k * 20), strictly native CC 24..31 in Mode 0 (Absolute)
    const size_t k1Offset = 117;
    for (size_t k = 0; k < 8; ++k) {
        size_t off = k1Offset + k * 20;
        if (off + 20 > packet.size()) break;
        packet[off + 0] = static_cast<uint8_t>(24 + k); // CC 24..31
        packet[off + 1] = 0x00;                         // Min 0
        packet[off + 2] = 0x7F;                         // Max 127
        packet[off + 3] = 0x00;                         // Mode 0 (Absolute)
        const std::string& lbl = knobLabels[k];
        for (size_t n = 0; n < 16; ++n) {
            packet[off + 4 + n] = (n < lbl.size()) ? (static_cast<uint8_t>(lbl[n]) & 0x7F) : 0x00;
        }
    }

    return packet;
}

// -----------------------------------------------------------------------------
// High-Level Instance Methods
// -----------------------------------------------------------------------------

bool DisplayDriver::takeScreenOwnership(OwnerType owner) {
    if (!pluginPortSender_) return false;
    if (!screenTakeoverEnabled_) {
        selectHardwareBank(false);
        return true;
    }

#if defined(MPK_DISPLAY_WITH_RTMIDI)
    if (rtPluginOut_ && !rtIsPluginPort_) {
        owner = OwnerType::Daw;
    }
#endif

    sendToPluginPort(makeScreenOwnerSysEx(owner, "MK4-SYNTH-SVK0", deviceId_));
    ownershipLocked_ = true;
    selectHardwareBank(false); // Default hardware to Bank A (White LED) upon initialization
    return true;
}

bool DisplayDriver::releaseScreenOwnership() {
    if (!pluginPortSender_) return false;
    sendToPluginPort(makeScreenOwnerSysEx(OwnerType::Release, "MK4-SYNTH-SVK0", deviceId_));
    ownershipLocked_ = false;
    invalidateScreen();
    return true;
}

bool DisplayDriver::writeLine(uint8_t lineIndex, const LineSpec& spec) {
    if (!screenTakeoverEnabled_) return false;
    return writeLineRaw(lineIndex, spec);
}

bool DisplayDriver::writeLineRaw(uint8_t lineIndex, const LineSpec& spec) {
    if (!pluginPortSender_ || lineIndex >= 5) return false;
    if (lineIndex < 3) {
        lastLines_[lineIndex] = spec;
    }
    sendToPluginPort(makeLineFormatSysEx(lineIndex, spec.align, spec.fg, spec.bg, spec.layout, deviceId_));
    sendToPluginPort(makeLineTextSysEx(lineIndex, spec.text, spec.layout, deviceId_));
    return true;
}

bool DisplayDriver::writeScreen3Lines(const LineSpec& line0, const LineSpec& line1, const LineSpec& line2,
                                      bool ensureOwnership) {
    if (!pluginPortSender_ || !screenTakeoverEnabled_) return false;
    if (ensureOwnership && !ownershipLocked_) {
        takeScreenOwnership(OwnerType::PluginSVK0);
    }
    writeLine(0, line0);
    writeLine(1, line1);
    writeLine(2, line2);
    return true;
}

bool DisplayDriver::writeText3Lines(const std::string& line0, const std::string& line1, const std::string& line2,
                                    Color bg, Align align0, Align align1, Align align2,
                                    bool ensureOwnership) {
    if (!screenTakeoverEnabled_) return false;
    Color subBg = bg.dimmed(0.45f);
    LineSpec s0(line0, align0, Color::White(), bg,    Layout::ThreeLineHeader);
    LineSpec s1(line1, align1, Color::Cyan(),  subBg, Layout::ThreeLineNormal);
    LineSpec s2(line2, align2, Color::Gold(),  subBg, Layout::ThreeLineNormal);
    return writeScreen3Lines(s0, s1, s2, ensureOwnership);
}

void DisplayDriver::invalidateScreen() {
    deferredHostDraw_ = false;
    screenMode_  = ScreenMode::None;
    stripValid_  = false;
    barSplitX_   = -1;
    hostValueText_.clear();
}

bool DisplayDriver::claimScreenIfNeeded() {
    if (!pluginPortSender_ || nativeUiOpen_) return false;
    if (!ownershipLocked_) {
        if (screenTakeoverEnabled_) {
            takeScreenOwnership(OwnerType::PluginSVK0);
        } else {
            // Claim without 0x3A bank select (that would reset the bank LED / hardware bank)
            sendToPluginPort(makeScreenOwnerSysEx(OwnerType::PluginSVK0, "MK4-SYNTH-SVK0", deviceId_));
            ownershipLocked_ = true;
        }
        invalidateScreen();
    }
    return true;
}

// Every 0x14/0x10 write makes the MK4 clear and redraw that line, so only lines that actually changed are resent
bool DisplayDriver::writeLineIfChanged(uint8_t lineIndex, const LineSpec& spec, bool force) {
    const LineSpec& last = lastLines_[lineIndex];
    if (!force && last.text == spec.text && last.align == spec.align && last.layout == spec.layout &&
        last.fg == spec.fg && last.bg == spec.bg) {
        return false;
    }
    return writeLineRaw(lineIndex, spec);
}

// The large layout-3 face fits about 11 characters across 160 px ("CS-80 BLADE" fills it; a 13-character name is
// clipped and starts the MK4 marquee). Longer names fall back to the regular face, which fits about 14.
static Layout headerLayoutFor(const std::string& text) {
    return text.size() > 11 ? Layout::ThreeLineNormal : Layout::ThreeLineHeader;
}

// Host pixels (0x71) land in the framebuffer immediately, but the MK4 applies a claim and line rewrites later in its
// own render loop, and those fills wipe pixels blitted in between (seen on the ESP32: a value with only its lower rows
// drawn). So pixels that a pending MK4 repaint would cover wait for the next call, one caller write interval later;
// callers keep updating while hasDeferredHostDraw() is true.

bool DisplayDriver::showHomeScreen(const std::string& name, const std::string& info, const Theme& theme) {
    const bool claimed = !ownershipLocked_;
    if (!claimScreenIfNeeded()) return false;
    const bool fresh = screenMode_ == ScreenMode::None;
    writeLineIfChanged(0, LineSpec(name, Align::Center, theme.headerFg, theme.headerBg, headerLayoutFor(name)), fresh);
    writeLineIfChanged(1, LineSpec(info, Align::Center, theme.mutedFg, theme.bodyBg, Layout::ThreeLineNormal), fresh);
    writeLineIfChanged(2, LineSpec(theme.family, Align::Center, theme.accent, theme.bodyBg, Layout::ThreeLineNormal),
                       fresh || screenMode_ == ScreenMode::Knob);
    hostValueText_.clear();
    screenMode_       = ScreenMode::Home;
    deferredHostDraw_ = claimed; // strip after the claim has been applied
    if (claimed) return true;
    if (!stripValid_ || stripBg_ != theme.bodyBg) {
        paintStrip(theme.bodyBg);
    } else if (barSplitX_ >= 0) {
        fillRect(HostLayer::BAR_X, HostLayer::BAR_Y, HostLayer::BAR_W, HostLayer::BAR_H, theme.bodyBg);
    }
    barSplitX_ = -1;
    return true;
}

bool DisplayDriver::showKnobHud(const std::string& header, const std::string& paramLabel, const std::string& valueText,
                                int value7Bit, const Theme& theme) {
    const bool claimed = !ownershipLocked_;
    if (!claimScreenIfNeeded()) return false;
    const bool enter = screenMode_ != ScreenMode::Knob;
    writeLineIfChanged(0, LineSpec(header, Align::Center, theme.accent, theme.bodyBg, headerLayoutFor(header)), enter);
    writeLineIfChanged(1, LineSpec(paramLabel, Align::Center, theme.labelFg, theme.bodyBg, Layout::ThreeLineNormal), enter);
    // Line 2 stays empty: the MK4 fills it with bodyBg once, the host draws the value on top
    const bool valueRowReset =
        writeLineIfChanged(2, LineSpec("", Align::Center, theme.valueFg, theme.bodyBg, Layout::ThreeLineNormal), enter);
    screenMode_ = ScreenMode::Knob;
    if (claimed || valueRowReset) {
        // Value (and after a claim also strip + bar) wait until the MK4 has repainted
        std::fill(valueShadow_.begin(), valueShadow_.end(), theme.bodyBg.rgb565());
        hostValueText_.clear();
        deferredHostDraw_ = true;
        if (claimed) return true;
    } else {
        drawHostValue(valueText, theme, deferredHostDraw_);
        deferredHostDraw_ = false;
    }
    if (!stripValid_ || stripBg_ != theme.bodyBg) paintStrip(theme.bodyBg);
    if (value7Bit >= 0) {
        drawValueBar(value7Bit, theme, enter);
    } else if (barSplitX_ >= 0) {
        fillRect(HostLayer::BAR_X, HostLayer::BAR_Y, HostLayer::BAR_W, HostLayer::BAR_H, theme.bodyBg);
        barSplitX_ = -1;
    }
    return true;
}

void DisplayDriver::fillRect(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    // Keep each packet around 1.5 KB so the ESP32 USB host queue never has to hold one huge message
    const int rowsPerPacket = std::max(1, 500 / w);
    std::vector<uint16_t> px(static_cast<size_t>(w) * std::min(h, rowsPerPacket), c.rgb565());
    for (int y0 = 0; y0 < h; y0 += rowsPerPacket) {
        const int rows = std::min(rowsPerPacket, h - y0);
        sendToPluginPort(makeBlitSysEx(x, y + y0, w, rows, px.data(), deviceId_));
    }
}

void DisplayDriver::paintStrip(Color bg) {
    fillRect(0, HostLayer::STRIP_Y, HostLayer::WIDTH, HostLayer::STRIP_H, bg);
    stripBg_    = bg;
    stripValid_ = true;
    barSplitX_  = -1;
}

void DisplayDriver::drawValueBar(int value7Bit, const Theme& theme, bool force) {
    const int v     = std::min(127, std::max(0, value7Bit));
    const int split = HostLayer::BAR_X + (HostLayer::BAR_W * v + 63) / 127;
    const int end   = HostLayer::BAR_X + HostLayer::BAR_W;
    if (force || barSplitX_ < 0) {
        fillRect(HostLayer::BAR_X, HostLayer::BAR_Y, split - HostLayer::BAR_X, HostLayer::BAR_H, theme.accent);
        fillRect(split, HostLayer::BAR_Y, end - split, HostLayer::BAR_H, theme.track);
    } else if (split > barSplitX_) {
        fillRect(barSplitX_, HostLayer::BAR_Y, split - barSplitX_, HostLayer::BAR_H, theme.accent);
    } else if (split < barSplitX_) {
        fillRect(split, HostLayer::BAR_Y, barSplitX_ - split, HostLayer::BAR_H, theme.track);
    }
    barSplitX_ = split;
}

void DisplayDriver::drawHostValue(const std::string& text, const Theme& theme, bool force) {
    constexpr int W = HostLayer::WIDTH;
    constexpr int H = HostLayer::VALUE_H;
    const uint16_t bg = theme.bodyBg.rgb565();
    if (valueShadow_.size() != static_cast<size_t>(W * H)) {
        valueShadow_.assign(W * H, bg);
        valueScratch_.assign(W * H, bg);
        force = true;
    }
    if (force) std::fill(valueShadow_.begin(), valueShadow_.end(), bg); // the MK4 just cleared line 2 to bodyBg

    // Render: centred (digits are tabular, so the text only shifts when the digit count changes)
    std::fill(valueScratch_.begin(), valueScratch_.end(), bg);
    int width = 0;
    for (char c : text) {
        if (c >= OledFont::FIRST_CHAR && c <= OledFont::LAST_CHAR) width += OledFont::GLYPHS[c - OledFont::FIRST_CHAR].advance;
    }
    int pen = (W - width) / 2;
    const int top = (H - OledFont::LINE_HEIGHT) / 2;
    uint16_t ramp[16];
    for (int a = 0; a < 16; ++a) ramp[a] = theme.bodyBg.mix(theme.valueFg, a / 15.0f).rgb565();
    for (char c : text) {
        if (c < OledFont::FIRST_CHAR || c > OledFont::LAST_CHAR) continue;
        const OledFont::Glyph& g = OledFont::GLYPHS[c - OledFont::FIRST_CHAR];
        for (int gy = 0; gy < g.height; ++gy) {
            const int y = top + g.yOffset + gy;
            if (y < 0 || y >= H) continue;
            for (int gx = 0; gx < g.width; ++gx) {
                const int x = pen + g.xOffset + gx;
                if (x < 0 || x >= W) continue;
                const int i = gy * g.width + gx;
                const uint8_t byte = OledFont::BITMAP[g.offset + i / 2];
                const uint8_t a = (i & 1) ? (byte & 0x0F) : (byte >> 4);
                if (a) valueScratch_[y * W + x] = ramp[a];
            }
        }
        pen += g.advance;
    }

    // Blit only the bounding box of changed pixels
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (valueScratch_[y * W + x] != valueShadow_[y * W + x]) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
        }
    }
    hostValueText_ = text;
    if (x1 < 0) return;
    const int bw = x1 - x0 + 1;
    const int rowsPerPacket = std::max(1, 500 / bw);
    std::vector<uint16_t> px;
    for (int ya = y0; ya <= y1; ya += rowsPerPacket) {
        const int rows = std::min(rowsPerPacket, y1 - ya + 1);
        px.clear();
        for (int y = ya; y < ya + rows; ++y) {
            px.insert(px.end(), valueScratch_.begin() + y * W + x0, valueScratch_.begin() + y * W + x1 + 1);
            std::copy(valueScratch_.begin() + y * W + x0, valueScratch_.begin() + y * W + x1 + 1,
                      valueShadow_.begin() + y * W + x0);
        }
        sendToPluginPort(makeBlitSysEx(x0, HostLayer::VALUE_Y + ya, bw, rows, px.data(), deviceId_));
    }
}

bool DisplayDriver::probeScreen() {
    if (!pluginPortSender_) return false;
    probing_ = true;
    sendToPluginPort(makeScreenOwnerSysEx(OwnerType::PluginSVK0, "MK4-SYNTH-SVK0", deviceId_));
    return true;
}

bool DisplayDriver::endTransientHud() {
    if (!ownershipLocked_) return false;
    return releaseScreenOwnership();
}

bool DisplayDriver::clearScreen(Color bg) {
    return writeText3Lines("", "", "", bg, Align::Center, Align::Center, Align::Center, true);
}

bool DisplayDriver::handleSysEx(const uint8_t* data, size_t len) {
    if (!data || len < 8) return false;
    if (data[0] != 0xF0 || data[1] != MANUFACTURER_AKAI || data[3] != MODEL_MPK_MINI_IV) return false;
    uint8_t cmd = data[4];
    // Command 0x19: Screen Owner Heartbeat Ping from controller (F0 47 <dev> 5D 19 00 00 F7)
    // On physical MPK Mini IV hardware, the controller emits this ping every 2.0s.
    // If not acknowledged via Keepalive Pong within 3 cycles (6.0s), ownership is revoked
    // and display reverts to Prst2: PLUGIN.
    if (cmd == 0x19) {
        if (len == 8 && data[5] == 0x00 && data[6] == 0x00 && data[7] == 0xF7) {
            nativeUiOpen_ = false; // the device only pings while it believes the host owns the screen
            sendKeepalive();
            return true;
        }
        // F0 47 <dev> 5D 19 00 01 00 F7: device took the screen back for a built-in menu (RE doc 4.3).
        // Only meaningful while we own the screen; after our own 1C release the device does not announce anything.
        if (len == 9 && data[5] == 0x00 && data[6] == 0x01 && data[7] == 0x00 && data[8] == 0xF7) {
            if (ownershipLocked_ || probing_) {
                nativeUiOpen_    = true;
                ownershipLocked_ = false;
                invalidateScreen();
            }
            return true;
        }
        // F0 47 <dev> 5D 19 00 11 <type> <16-byte id> F7: we own the screen (ack of our claim, or the menu closed).
        // After a menu the device does NOT redraw the line buffers, so everything must be drawn again.
        if (len >= 10 && data[5] == 0x00 && data[6] == 0x11 && data[len - 1] == 0xF7) {
            if (nativeUiOpen_) {
                nativeUiOpen_    = false;
                ownershipLocked_ = true;
                invalidateScreen();
                if (screenTakeoverEnabled_) {
                    for (uint8_t i = 0; i < 3; ++i) {
                        if (!lastLines_[i].text.empty()) writeLineRaw(i, lastLines_[i]);
                    }
                } else {
                    needsRedraw_ = true;
                }
            }
            probing_ = false;
            return true;
        }
    }
    // Command 0x3B: OCT+/OCT- button press, F0 47 <dev> 5D 3B 00 01 <octave 0..8> F7
    if (cmd == 0x3B && len == 9 && data[5] == 0x00 && data[6] == 0x01 && data[8] == 0xF7) {
        if (data[7] <= 8) {
            keybedOctave_ = data[7];
            return true;
        }
        return false;
    }
    // Command 0x6A: parameter notification / query reply
    // F0 47 <dev> 5D 6A 00 03 <idxHi> <idxLo> <value> F7
    if (cmd == 0x6A && len == 11 && data[5] == 0x00 && data[6] == 0x03 && data[10] == 0xF7) {
        const uint16_t idx = static_cast<uint16_t>((data[7] << 7) | data[8]);
        const uint8_t value = data[9];
        if (idx == 0x02 && value <= 8) {
            keybedOctave_ = value;
            return true;
        }
        if (idx == 0x03 && value <= 24) {
            keybedTranspose_ = value;
            return true;
        }
    }
    return false;
}

bool DisplayDriver::requestKeybedState() {
    if (!pluginPortSender_) return false;
    sendToPluginPort(makeParamQuerySysEx(0x02, deviceId_));
    sendToPluginPort(makeParamQuerySysEx(0x03, deviceId_));
    return true;
}

bool DisplayDriver::sendKeepalive() {
    if (!pluginPortSender_) return false;
    sendToPluginPort(makeHeartbeatReplySysEx(OwnerType::PluginSVK0, "MK4-SYNTH-SVK0", deviceId_));
    return true;
}

bool DisplayDriver::setDeviceTempo(uint16_t bpm) {
    if (!pluginPortSender_) return false;
    sendToPluginPort(makeSetTempoSysEx(bpm, deviceId_));
    return true;
}

bool DisplayDriver::setKnobValue(uint8_t knobIndex, uint8_t value7Bit) {
    if (!pluginPortSender_) return false;
    sendToPluginPort(makeKnobValueSysEx(knobIndex, value7Bit, deviceId_));
    return true;
}

bool DisplayDriver::syncKnobValues(const std::array<uint8_t, 8>& values) {
    if (!pluginPortSender_) return false;
    for (uint8_t k = 0; k < 8; ++k) {
        sendToPluginPort(makeKnobValueSysEx(k, values[k], deviceId_));
#if (defined(MPK_DISPLAY_WITH_RTMIDI) || !defined(ARDUINO)) && !defined(MOCK_HAL)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
    }
    return true;
}

bool DisplayDriver::setPadColor(uint8_t padIndex, uint8_t colorIndex) {
    return setKnobValue(padIndex, colorIndex);
}

bool DisplayDriver::selectHardwareBank(bool isBankB) {
    if (!pluginPortSender_) return false;
    sendToPluginPort(makeBankSelectSysEx(isBankB, deviceId_));
    return true;
}


bool DisplayDriver::syncRamPreset(const std::string& programName,
                                  const std::array<std::string, 8>& knobLabels,
                                  uint8_t padColorIndex) {
    if (!swControlPortSender_) return false;
    auto packet = makeRamPresetSysEx(programName, knobLabels, padColorIndex, &baseRamTemplate_);
    sendToSwControlPort(packet);
    return true;
}

Theme Theme::fromAccent(Color accent, bool isBankB, uint8_t padColor, const char* family) {
    Theme t;
    t.accent   = accent;
    t.bodyBg   = Color::Black().mix(accent, 0.07f);
    t.labelFg  = Color::White().mix(accent, 0.12f);
    t.mutedFg  = Color(140, 140, 140).mix(accent, 0.25f);
    t.valueFg  = accent.mix(Color::White(), 0.35f);
    t.track    = t.bodyBg.mix(accent, 0.18f);
    t.padColor = padColor;
    t.family   = family;
    if (isBankB) {
        t.headerBg = Color::Black().mix(accent, 0.22f);
        t.headerFg = accent;
    } else {
        t.headerBg = accent;
        t.headerFg = accent.luma() > 140 ? Color::Black().mix(accent, 0.15f) : Color::White();
    }
    return t;
}

Theme DisplayDriver::themeForPresetName(const std::string& presetName, bool isBankB) {
    struct Entry {
        const char* keys[3];
        uint32_t    accent;
        uint8_t     padColor;
        const char* family;
    };
    // Ordered: more specific names first ("DX E-PIANO" must not match the Casio "PIANO")
    static const Entry kThemes[] = {
        {{"ODYSSEY"},                    0xE67E22, 10, "Sync lead"},
        {{"VINTAGE JUP", "JUPITER"},    0xE74C3C, 5,  "Analog poly"},
        {{"TUBESYNTH"},                  0xD35400, 9,  "Tube poly"},
        {{"SUB FACTORY"},                0x27AE60, 25, "Sub bass"},
        {{"MEMORYMOOG"},                 0x8E44AD, 49, "Analog poly"},
        {{"IONA"},                       0x16A085, 29, "Acid bass"},
        {{"OPX-4", "OPX"},              0x2980B9, 37, "FM poly"},
        {{"SOLINA"},                     0x9B59B6, 45, "String ensemble"},
        {{"STAGE PIANO"},                0x34495E, 13, "Grand piano"},
        {{"STUDIO STR"},                 0xC0392B, 41, "String section"},
        {{"FABRIC"},                     0x1ABC9C, 21, "Wavetable pad"},
        {{"MARIANA"},                    0x2C3E50, 17, "Dual bass"},
        {{"HYPE"},                       0xF39C12, 61, "Hypersaw"},
        {{"JUNO"},                       0xFF6A1F, 5,  "Analog poly"},
        {{"MOOG", "MINI"},               0xFFB02E, 9,  "Analog mono"},
        {{"PROPHET"},                    0xB46CFF, 49, "Analog poly"},
        {{"303", "ACID"},                0x9BE22D, 25, "Acid bass"},
        {{"CS-80", "CS80"},              0xFF3050, 33, "Analog poly"},
        {{"OB-X", "OBERHEIM"},           0x3D8BFF, 41, "Analog poly"},
        {{"DX", "E-PIANO"},              0x2ED3BF, 37, "FM keys"},
        {{"TINES", "CRYSTAL"},           0x8FD8FF, 37, "FM keys"},
        {{"RHODES", "WURLI"},            0xE8A868, 13, "FM keys"},
        {{"CLAV"},                       0xF2D23C, 17, "FM keys"},
        {{"PPG", "WAVE"},                0x34E0FF, 21, "Wavetable"},
        {{"CZ "},                        0xFF4FD8, 57, "Phase dist"},
        {{"KS ", "GUITAR", "KOTO"},      0xD9B26F, 61, "Plucked string"},
        {{"B3 "},                        0xB5482B, 15, "Drawbar organ"},
        {{"ORGAN"},                      0xC8643C, 3,  "FM organ"},
        {{"PIANO"},                      0xFF6FB0, 53, "Lo-fi keys"},
        {{"FANTASY", "FNTSY"},           0x9C8CFF, 45, "Lo-fi keys"},
        {{"VIOLIN", "VIOLN"},            0xE89A5C, 11, "Lo-fi keys"},
        {{"FLUTE"},                      0x5FE0B0, 29, "Lo-fi keys"},
    };

    const std::string p = toUpperAscii(presetName);
    Color       accent = Color::hex(0x6FA8DC);
    uint8_t     pad    = 41;
    const char* family = "User patch";
    for (const Entry& e : kThemes) {
        bool hit = false;
        for (const char* key : e.keys) hit = hit || (key && p.find(key) != std::string::npos);
        if (hit) {
            accent = Color::hex(e.accent);
            pad    = e.padColor;
            family = e.family;
            break;
        }
    }
    if (isBankB) {
        pad = static_cast<uint8_t>((pad + 12) % 64);
        if (pad == 0) pad = 17;
    }
    return Theme::fromAccent(accent, isBankB, pad, family);
}

} // namespace MpkMiniMk4
