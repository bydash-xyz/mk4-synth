#pragma once

/**
 * MpkMiniMk4Display.h
 * Standalone Generic-Purpose OLED Screen & Hardware Controller Library for Akai MPK Mini MK4 (IV)
 *
 * Implements the MPK Mini IV's screen and control SysEx, as observed on the device for interoperability:
 *   - 0x3A: Select Active Hardware Mode (0x00=DAW, 0x01=Plugin, 0x02..0x09=User 1..8)
 *   - 0x1C: Take/Release Screen Ownership (ownerType=0x01 "SVK0" on Plugin Port, 0x02 "DAW" on DAW Port)
 *   - 0x19: Bidirectional Screen Ownership Heartbeat
 *   - 0x2D: Query Active Screen Owner
 *   - 0x14: Set Line Format, Horizontal Alignment (Left=0, Center=1, Right=2), and 16-bit RGB565 FG/BG Colors
 *   - 0x11: Set Line Background from Akai 16-Color Hardware Palette + Auto-Contrast FG
 *   - 0x10: Set Line Text (up to 31 ASCII chars; auto-triggers hardware marquee scroll when width >= 160px)
 *   - 0x12: Set Individual RGB Pad LED Color (Pads 0..7, Color 0..127)
 *   - 0x71: Raw RGB565 pixel blit into the 160x80 framebuffer (host-drawn knob value + value bar)
 *   - 0x64: Write 284-Byte Active RAM Preset (Slot 0) for Knob Labels (CC 24..31), Program Name, and Pad Theme
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if defined(MPK_DISPLAY_WITH_RTMIDI)
#include <memory>
#include <thread>
#include <chrono>
#include <rtmidi/RtMidi.h>
#endif

namespace MpkMiniMk4 {

// Horizontal text alignment on the 160x80 pixel OLED
enum class Align : uint8_t {
    Left   = 0x00, // x = 0
    Center = 0x01, // x = (160 - textWidth) / 2
    Right  = 0x02  // x = 160 - textWidth
};

// Vertical layout & font mode
enum class Layout : uint8_t {
    FiveLineNormal   = 0x00, // 5 lines x 16px height (normal)
    FiveLineInverted = 0x01, // 5 lines x 16px height (highlighted cursor)
    ThreeLineNormal  = 0x02, // 3 lines x 24px height (standard text)
    ThreeLineHeader  = 0x03  // 3 lines x 24px height (header banner / bold)
};

// Active hardware mode selected via command 0x3A on Plugin Port
enum class HardwareMode : uint8_t {
    Daw    = 0x00,
    Plugin = 0x01, // Required for Plugin Port (SVK0) OLED buffer to be displayed
    User1  = 0x02,
    User2  = 0x03,
    User3  = 0x04,
    User4  = 0x05,
    User5  = 0x06,
    User6  = 0x07,
    User7  = 0x08,
    User8  = 0x09
};

// Port-specific screen owner type for command 0x1C
enum class OwnerType : uint8_t {
    Release         = 0x00,
    PluginSVK0      = 0x01, // Required when sending 0x1C on "MPK mini IV Plugin Port" (USB Cable 2)
    Daw             = 0x02, // Required when sending 0x1C on "MPK mini IV DAW Port" (USB Cable 1)
    SoftwareControl = 0x03  // Required when sending 0x1C on "MPK mini IV Software Control Port" (USB Cable 3)
};

// 8-bit RGB color. The panel is RGB565: 0x14 / 0x71 packets carry R and B as 0..31 and G as 0..63
// (the device packs them with no masking), so conversion happens only in r5()/g6()/b5().
struct Color {
    uint8_t r{255};
    uint8_t g{255};
    uint8_t b{255};

    constexpr Color() = default;
    constexpr Color(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}

    static constexpr Color hex(uint32_t rgb) {
        return {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
    }
    // From device-native 5-6-5 channel values (R 0..31, G 0..63, B 0..31)
    static constexpr Color fromNative(uint8_t r5, uint8_t g6, uint8_t b5) {
        return {static_cast<uint8_t>((r5 & 0x1F) * 255 / 31), static_cast<uint8_t>((g6 & 0x3F) * 255 / 63),
                static_cast<uint8_t>((b5 & 0x1F) * 255 / 31)};
    }

    constexpr uint8_t  r5() const { return static_cast<uint8_t>(r >> 3); }
    constexpr uint8_t  g6() const { return static_cast<uint8_t>(g >> 2); }
    constexpr uint8_t  b5() const { return static_cast<uint8_t>(b >> 3); }
    constexpr uint16_t rgb565() const { return static_cast<uint16_t>((r5() << 11) | (g6() << 5) | b5()); }

    // Linear mix towards `other` (t = 0 -> this, t = 1 -> other)
    constexpr Color mix(Color other, float t) const {
        return {static_cast<uint8_t>(r + (other.r - r) * t + 0.5f), static_cast<uint8_t>(g + (other.g - g) * t + 0.5f),
                static_cast<uint8_t>(b + (other.b - b) * t + 0.5f)};
    }
    constexpr Color dimmed(float factor = 0.45f) const { return Black().mix(*this, factor); }
    // Rec. 601 luma, 0..255
    constexpr int luma() const { return (r * 299 + g * 587 + b * 114) / 1000; }

    constexpr bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
    constexpr bool operator!=(const Color& o) const { return !(*this == o); }

    static constexpr Color White()   { return {255, 255, 255}; }
    static constexpr Color Black()   { return {0,   0,   0};   }
    static constexpr Color Cyan()    { return hex(0x7FFFFF); }
    static constexpr Color Gold()    { return hex(0xFFDC28); }
    static constexpr Color Crimson() { return hex(0xF01058); }
    static constexpr Color Amber()   { return hex(0xE07008); }
    static constexpr Color Emerald() { return hex(0x10C040); }
    static constexpr Color Teal()    { return hex(0x10A8E0); }
    static constexpr Color Cobalt()  { return hex(0x1848FF); }
};

// Per-line formatting and text specification
struct LineSpec {
    std::string text;
    Align       align{Align::Left};
    Layout      layout{Layout::ThreeLineNormal};
    Color       fg{Color::White()};
    Color       bg{Color::Black()};

    LineSpec() = default;
    LineSpec(std::string t, Align a = Align::Left, Color fgColor = Color::White(),
             Color bgColor = Color::Black(), Layout l = Layout::ThreeLineNormal)
        : text(std::move(t)), align(a), layout(l), fg(fgColor), bg(bgColor) {}
};

// Screen theme for a synth: one accent color identifies the instrument, everything else is derived from it
// so every synth shares the same hierarchy (banner > value > label > secondary info).
struct Theme {
    Color       accent{Color::hex(0x6FA8DC)};
    Color       headerBg{accent};           // home-screen banner
    Color       headerFg{Color::Black()};
    Color       bodyBg{Color::Black()};     // near-black with a hint of the accent
    Color       labelFg{Color::White()};    // parameter name
    Color       valueFg{Color::White()};    // parameter value (host-drawn)
    Color       mutedFg{Color::White()};    // secondary info (slot / bank)
    Color       track{Color::Black()};      // unfilled part of the value bar
    uint8_t     padColor{41};
    const char* family{"Synth"};            // short tag on the home screen, <= 12 chars

    // Bank B inverts the banner (accent text on a dark tint) instead of shifting the hue
    static Theme fromAccent(Color accent, bool isBankB = false, uint8_t padColor = 41, const char* family = "Synth");
};

// Screen geometry for the 3-line layouts (layout 2/3: 24 px rows at y = 0, 24, 48). Rows 72..79 are not covered
// by any line, so the value bar lives there; the value itself is drawn by the host over an empty line 2.
struct HostLayer {
    static constexpr int WIDTH   = 160;
    static constexpr int VALUE_Y = 48;
    static constexpr int VALUE_H = 24;
    static constexpr int STRIP_Y = 72;
    static constexpr int STRIP_H = 8;
    static constexpr int BAR_X   = 8;
    static constexpr int BAR_Y   = 73;
    static constexpr int BAR_W   = 144;
    static constexpr int BAR_H   = 4;
};

// Decoded Universal Device Inquiry Reply (F0 7E <devId> 06 02 47 5D 00 19 <fw0..3> <id> <hw0..3> <serial16> F7)
struct DeviceIdentity {
    bool                 valid{false};
    uint8_t              deviceId{0x00};           // Byte 2: SysEx channel/device ID
    uint8_t              manufacturerId{0x00};     // Byte 5: 0x47 (Akai Professional)
    uint8_t              productId{0x00};          // Byte 6: 0x5D (MPK mini IV / MK4)
    uint16_t             familyMemberCode{0};      // Bytes 7..8: 0x0019 (Hardware Family/Member ID)
    uint8_t              fwMajor{0};               // Byte 9
    uint8_t              fwMinor{0};               // Byte 10
    uint8_t              fwPatch{0};               // Byte 11
    uint8_t              fwBuild{0};               // Byte 12
    uint8_t              sysexDeviceId{0};         // Byte 13
    std::array<uint8_t, 4> hardwareRevBytes{{0, 0, 0, 0}}; // Bytes 14..17
    std::string          serialNumber;             // Bytes 18..33 (16-byte ASCII hardware serial)
    std::vector<uint8_t> rawReply;                 // Raw 35-byte 06 02 reply

    bool isMpkMiniMk4() const {
        return valid && manufacturerId == 0x47 && productId == 0x5D;
    }

    // Checks compatibility with the Studio Instrument Collection (0x3A / 0x1C / 0x14 / 0x10) OLED protocol
    bool isFirmwareCompatible() const {
        if (!isMpkMiniMk4()) return false;
        return (fwMajor >= 1) || (fwMinor > 0) || (fwPatch > 0) || (fwBuild > 0);
    }

    std::string firmwareVersionString() const;
    std::string hardwareVersionString() const;
};

class DisplayDriver {
public:
    using SendPacketFn = std::function<void(const uint8_t* data, size_t len)>;

    static constexpr uint8_t MANUFACTURER_AKAI = 0x47;
    static constexpr uint8_t MODEL_MPK_MINI_IV = 0x5D;
    static constexpr size_t  RAM_PRESET_SIZE   = 284;

    explicit DisplayDriver(uint8_t deviceId = 0x00);
    ~DisplayDriver() = default;

    // -------------------------------------------------------------------------
    // Transport & Hardware Identity Verification
    // -------------------------------------------------------------------------
    void setPluginPortSender(SendPacketFn fn) { pluginPortSender_ = std::move(fn); }
    void setSoftwareControlPortSender(SendPacketFn fn) { swControlPortSender_ = std::move(fn); }
    void setBasePresetTemplate(const std::vector<uint8_t>& tpl) { baseRamTemplate_ = tpl; }
    bool hasPluginPortSender() const { return static_cast<bool>(pluginPortSender_); }
    bool hasSoftwareControlPortSender() const { return static_cast<bool>(swControlPortSender_); }
    bool isConnected() const {
#if defined(MPK_DISPLAY_WITH_RTMIDI)
        if (rtPluginOut_ != nullptr) return true;
#endif
        return hasPluginPortSender() || hasSoftwareControlPortSender();
    }

    // Decodes a 35-byte Universal Device Inquiry Reply (F0 7E <dev> 06 02 47 5D ...)
    static DeviceIdentity parseDeviceIdentityReply(const uint8_t* data, size_t len);

#if defined(MPK_DISPLAY_WITH_RTMIDI)
    // Automatically discovers, verifies hardware/firmware identity, and opens MPK mini IV ports
    bool autoConnectRtMidi();
    // Actively queries Universal Device Inquiry (F0 7E 7F 06 01 F7) and populates outIdentity
    bool queryDeviceIdentity(DeviceIdentity& outIdentity);
#endif

    // -------------------------------------------------------------------------
    // High-Level Generic OLED Screen Operations
    // -------------------------------------------------------------------------
    void setScreenTakeoverEnabled(bool enabled) { screenTakeoverEnabled_ = enabled; }
    bool isScreenTakeoverEnabled() const { return screenTakeoverEnabled_; }

    // Sends 0x3A (Plugin mode) + 0x1C (SVK0 screen ownership) on Plugin Port
    bool takeScreenOwnership(OwnerType owner = OwnerType::PluginSVK0);

    // Releases screen ownership (0x1C with ownerType = 0x00)
    bool releaseScreenOwnership();

    // Writes a single formatted line (0x14 format + 0x10 text) to the OLED
    bool writeLine(uint8_t lineIndex, const LineSpec& spec);

    // Writes a 3-line screen with per-line text, alignment (Left/Center/Right), and RGB565 colors
    bool writeScreen3Lines(const LineSpec& line0, const LineSpec& line1, const LineSpec& line2,
                           bool ensureOwnership = true);

    // Convenience overload for 3 strings + background color + per-line alignments
    bool writeText3Lines(const std::string& line0, const std::string& line1, const std::string& line2,
                         Color bg = Color::Crimson(),
                         Align align0 = Align::Center, Align align1 = Align::Left, Align align2 = Align::Right,
                         bool ensureOwnership = true);

    // Home screen: synth name on an accent banner, muted info line (slot / bank), family tag. All MK4 text.
    // Claims the screen if needed (also while screenTakeover is disabled); only lines that changed are resent.
    bool showHomeScreen(const std::string& name, const std::string& info, const Theme& theme);

    // Knob screen: quiet header, parameter name, and a host-drawn value (0x71 blits of only the pixels that
    // changed; the MK4's own text redraw clears the line first and flickers) plus a value bar when value7Bit >= 0.
    bool showKnobHud(const std::string& header, const std::string& paramLabel, const std::string& valueText,
                     int value7Bit, const Theme& theme);

    // Hands the screen back to the MK4 firmware (1C release)
    bool endTransientHud();
    const std::string& getHostValueText() const { return hostValueText_; }
    // True when host pixels were held back because the MK4 still has to apply a claim or line 2 rewrite: call the same
    // screen function again (after >= one write interval) to draw them
    bool hasDeferredHostDraw() const { return deferredHostDraw_; }

    // True while a built-in MK4 screen (Scale/Chord Setup via SHIFT+Pad, Global, Load/Save Program...) has taken the
    // display back from us (device sent 19 00 01 00). Overlays must not claim the screen until the device resumes (19 00 11)
    // or pings (19 00 00), otherwise the native menu closes under the user's hands.
    bool isNativeUiOpen() const { return nativeUiOpen_; }
    // While the device owns the screen, ask for it back (1C claim). The device answers 19 00 11 if no menu is open
    // (-> nativeUiOpen_ clears and needsRedraw is raised) or 19 00 01 00 if a menu still is.
    bool probeScreen();
    // True once after the screen came back from the device: every line must be redrawn (the MK4 does not)
    bool consumeNeedsRedraw() { bool r = needsRedraw_; needsRedraw_ = false; return r; }

    // Handles incoming SysEx packet received from the controller on Plugin Port or MIDI Port.
    // When a Screen Owner Heartbeat Ping (CMD 0x19) is received, automatically responds with Keepalive Pong.
    bool handleSysEx(const uint8_t* data, size_t len);

    // Keybed octave / transpose as reported by the controller (SysEx 0x6A parameter
    // notifications, param idx 0x02 = octave 0..8 with 4 = centre, idx 0x03 = transpose
    // 0..24 with 12 = none). The MPK applies both itself, so notes arrive pre-shifted;
    // getKeybedNoteOffset() is the semitone shift to subtract to recover the physical key.
    uint8_t getKeybedOctave() const { return keybedOctave_; }
    int     getKeybedTranspose() const { return static_cast<int>(keybedTranspose_) - 12; }
    int     getKeybedNoteOffset() const { return (static_cast<int>(keybedOctave_) - 4) * 12 + getKeybedTranspose(); }

    // (OCT+/OCT- presses arrive as SysEx 0x3B with the new octave; 0x6A carries queried values.)
    // Asks the controller for its current octave + transpose (SysEx 0x6A query, len 2).
    bool requestKeybedState();

    // Proactively sends a Screen Owner Keepalive Pong over Plugin Port to refresh controller lease timer
    bool sendKeepalive();

    // Clears all 3 lines to the specified background color
    bool clearScreen(Color bg = Color::Black());

    // Sets a single hardware rotary encoder accumulator (knobIndex: 0..7, value7Bit: 0..127) via SysEx 0x12
    bool setKnobValue(uint8_t knobIndex, uint8_t value7Bit);

    // Sets all 8 hardware rotary encoder accumulators (0..7) to the given 7-bit values (8 x 10 bytes = 80 bytes)
    bool syncKnobValues(const std::array<uint8_t, 8>& values);

    // Sets the MK4's own tempo (TAP LED, arp, note repeat) via SysEx 0x6C; the device accepts 30..240 BPM
    bool setDeviceTempo(uint16_t bpm);

    // Sets a single RGB pad LED color / knob value via command 0x12 (backward compatible alias)
    bool setPadColor(uint8_t padIndex, uint8_t colorIndex);

    // Synchronizes the 284-byte Active RAM preset (Slot 0) on Software Control Port (0x64)
    // Updates Program Name, 8 Knob Labels (strictly CC 24..31 Absolute), and 16 Pad Colors
    bool syncRamPreset(const std::string& programName,
                       const std::array<std::string, 8>& knobLabels,
                       uint8_t padColorIndex = 41);

    // Hardware Bank Selection (Command 0x3A; MK4 accepts it only on USB-MIDI virtual cable 2)
    // Selects Bank A (isBankB = false, illuminates button White) or Bank B (isBankB = true, illuminates button Red)
    bool selectHardwareBank(bool isBankB = false);

    // Resolves a distinct color theme for a synth preset name & bank ("A" / "B")
    static Theme themeForPresetName(const std::string& presetName, bool isBankB = false);

    // -------------------------------------------------------------------------
    // Pure Static SysEx Packet Builders (Zero-Allocation / Testable)
    // -------------------------------------------------------------------------
    static std::vector<uint8_t> makeDeviceInquirySysEx();
    // Parameter query: F0 47 <dev> 5D 6A 00 02 <idxHi> <idxLo> F7 (reply is a 0x6A notification)
    static std::vector<uint8_t> makeParamQuerySysEx(uint16_t paramIdx, uint8_t devId = 0x00);
    static std::vector<uint8_t> makeQueryOwnerSysEx(uint8_t devId = 0x00);
    static std::vector<uint8_t> makeSelectModeSysEx(HardwareMode mode = HardwareMode::Plugin, uint8_t devId = 0x00);
    static std::vector<uint8_t> makeScreenOwnerSysEx(OwnerType owner = OwnerType::PluginSVK0,
                                                     const std::string& uuid16 = "MK4-SYNTH-SVK0",
                                                     uint8_t devId = 0x00);
    static std::vector<uint8_t> makeHeartbeatReplySysEx(OwnerType owner = OwnerType::PluginSVK0,
                                                        const std::string& uuid16 = "MK4-SYNTH-SVK0",
                                                        uint8_t devId = 0x00);
    static std::vector<uint8_t> makeLineFormatSysEx(uint8_t line, Align align,
                                                    Color fg, Color bg,
                                                    Layout layout = Layout::ThreeLineNormal,
                                                    uint8_t devId = 0x00);
    static std::vector<uint8_t> makePaletteLineColorSysEx(uint8_t line, uint8_t paletteIdx, uint8_t devId = 0x00);
    static std::vector<uint8_t> makeLineTextSysEx(uint8_t line, const std::string& text,
                                                  Layout layout = Layout::ThreeLineNormal,
                                                  uint8_t devId = 0x00);

    // Raw pixel blit into the 160x80 framebuffer (hardware-verified; accepted on any cable):
    // F0 47 <dev> 5D 71 <lenHi> <lenLo> <xHi xLo> <yHi yLo> <wHi wLo> <hHi hLo> (<R5> <G6> <B5>) x w*h F7
    // Coordinates are 14-bit (hi = upper 7 bits); pixel i lands at (x + i % w, y + i / w). Pixels are RGB565.
    static std::vector<uint8_t> makeBlitSysEx(int x, int y, int w, int h, const uint16_t* pixels,
                                              uint8_t devId = 0x00);

    // Builds a 10-byte SysEx 0x12 packet to set hardware rotary encoder accumulator:
    // F0 47 <devId> 5D 12 00 02 <knobIdx: 0..7> <knobValue: 0..127> F7
    static std::vector<uint8_t> makeKnobValueSysEx(uint8_t knobIndex, uint8_t value7Bit, uint8_t devId = 0x00);

    // F0 47 <devId> 5D 6C 00 02 <bpm >> 7> <bpm & 0x7F> F7
    static std::vector<uint8_t> makeSetTempoSysEx(uint16_t bpm, uint8_t devId = 0x00);

    // Builds a 9-byte SysEx 0x3A packet to set hardware Bank & Button LED on MPK Mini IV:
    // F0 47 <devId> 5D 3A 00 01 <0x01 for Bank A (White), 0x00 for Bank B (Red)> F7
    static std::vector<uint8_t> makeBankSelectSysEx(bool isBankB = false, uint8_t devId = 0x00);
    static std::vector<uint8_t> makePadColorSysEx(uint8_t padIndex, uint8_t colorIndex, uint8_t devId = 0x00);
    static std::vector<uint8_t> makeRamPresetSysEx(const std::string& programName,
                                                   const std::array<std::string, 8>& knobLabels,
                                                   uint8_t padColorIndex = 41,

                                                   const std::vector<uint8_t>* baseTemplate = nullptr);

    const std::array<LineSpec, 3>& getLastLines() const { return lastLines_; }
    bool isOwnershipLocked() const { return ownershipLocked_; }

private:
    enum class ScreenMode : uint8_t { None, Home, Knob };

    bool writeLineRaw(uint8_t lineIndex, const LineSpec& spec);
    bool writeLineIfChanged(uint8_t lineIndex, const LineSpec& spec, bool force);
    bool claimScreenIfNeeded();
    void invalidateScreen(); // after a claim/release/menu the device content is unknown: redraw everything
    void sendToPluginPort(const std::vector<uint8_t>& packet);
    void sendToSwControlPort(const std::vector<uint8_t>& packet);

    // Host-drawn layer (value text + bar), see HostLayer
    void fillRect(int x, int y, int w, int h, Color c);
    void paintStrip(Color bg);
    void drawValueBar(int value7Bit, const Theme& theme, bool force);
    void drawHostValue(const std::string& text, const Theme& theme, bool force);

    uint8_t                  deviceId_{0x00};
    uint8_t                  keybedOctave_{4};
    uint8_t                  keybedTranspose_{12};
    bool                     screenTakeoverEnabled_{true};
    bool                     ownershipLocked_{false};
    ScreenMode               screenMode_{ScreenMode::None};
    bool                     stripValid_{false};    // rows 72..79 painted with stripBg_ since the last claim
    Color                    stripBg_{};
    int                      barSplitX_{-1};        // first unfilled bar column, -1 = bar not drawn
    std::string              hostValueText_{};
    bool                     deferredHostDraw_{false};
    std::vector<uint16_t>    valueShadow_{};        // what the MK4 shows in the value rows (lazily allocated)
    std::vector<uint16_t>    valueScratch_{};
    bool                     nativeUiOpen_{false};
    bool                     probing_{false};
    bool                     needsRedraw_{false};
    DeviceIdentity           deviceIdentity_{};
    std::array<LineSpec, 3>  lastLines_{};
    std::vector<uint8_t>     baseRamTemplate_{};
    SendPacketFn             pluginPortSender_{};
    SendPacketFn             swControlPortSender_{};

#if defined(MPK_DISPLAY_WITH_RTMIDI)
    std::unique_ptr<RtMidiOut> rtPluginOut_{};
    std::unique_ptr<RtMidiOut> rtSwControlOut_{};
    uint8_t                    rtOwnerType_{static_cast<uint8_t>(OwnerType::PluginSVK0)};
    bool                       rtIsPluginPort_{true};
#endif
};

} // namespace MpkMiniMk4
