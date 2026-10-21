#pragma once

#include "Parameters.h"
#include "Presets.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <string>

namespace Synth {

constexpr uint8_t SYSEX_START           = 0xF0;
constexpr uint8_t SYSEX_END             = 0xF7;
constexpr uint8_t SYSEX_MANUFACTURER_ID = 0x7D; // Educational / Research
constexpr uint8_t SYSEX_FAMILY_ID       = 0x01; // mk4-synth family

enum class SysExCommand : uint8_t {
    FastParamUpdate = 0x01,
    PatchDump       = 0x02,
    SaveUserSlot    = 0x03,
    RequestDump     = 0x04,
    PatchDumpV2     = 0x05, // Parameter-list patch dump (ADR-0006)
    Handshake       = 0x0F
};

enum class HandshakeStatus : uint8_t {
    AckSuccess       = 0x00,
    NackChecksumErr  = 0x01,
    NackInvalidSlot  = 0x02,
    NackCorrupted    = 0x03,
    NackUnsupported  = 0x04
};

enum class ParamId : uint8_t {
    // Core Oscillator & Filter
    Cutoff            = 0x01,
    Resonance         = 0x02,
    Waveform          = 0x03,
    SubOscLevel       = 0x04,
    DetuneCents       = 0x05,
    Drive             = 0x06,
    FilterEnvAmount   = 0x07,
    EngineMode        = 0x08, // EngineMode index: 0 subtractive, 1 FM, 2 wavetable, 3 phase dist, 4 pluck, 5 additive
    Voicing           = 0x09, // Factory SynthModel index + 1; 0 = generic user voicing

    // Amp Envelope
    AmpAttack         = 0x10,
    AmpDecay          = 0x11,
    AmpSustain        = 0x12,
    AmpRelease        = 0x13,

    // Filter Envelope
    FilterAttack      = 0x14,
    FilterDecay       = 0x15,
    FilterSustain     = 0x16,
    FilterRelease     = 0x17,

    // Chorus
    ChorusEnabled     = 0x20,
    ChorusRate        = 0x21,
    ChorusDepth       = 0x22,
    ChorusMix         = 0x23,

    // Voice Mode & Portamento
    VoiceMode         = 0x30,
    GlideTime         = 0x31,
    PortamentoEnabled = 0x32,
    LegatoEnabled     = 0x33,

    // Master
    MasterVolume       = 0x40,

    // Metronome, Time Signature & Key/Scale
    MetronomeEnabled   = 0x50,
    MetronomeBpm       = 0x51,
    TimeSigNumerator   = 0x52,
    TimeSigDenominator = 0x53,
    RootKey            = 0x54,
    ScaleMode          = 0x55,

    // 2-Operator FM
    ModRatio           = 0x60,
    ModDepth           = 0x61,
    ModFeedback        = 0x62,
    TremoloDepth       = 0x63,
    TremoloRate        = 0x64,

    // Wavetable / Phase Distortion / Pluck / Additive (TimbreParams)
    WtTable            = 0x65,
    WtPosition         = 0x66,
    WtEnvAmount        = 0x67,
    PdWave             = 0x68,
    PdDcw              = 0x69,
    PluckDecay         = 0x6A,
    PluckBright        = 0x6B,
    PluckPick          = 0x6C,
    PercLevel          = 0x6D,
    PercHarmonic       = 0x6E,
    KeyClick           = 0x6F,
    Drawbar1           = 0x70, // 16'; Drawbar1 + i for the nine drawbars (0x70..0x78)
    Drawbar9           = 0x78, // 1'
    OpxAlgo            = 0x79  // OPx4 algorithm (0..7)
};

// Patch-scope parameters: the single source of truth for ranges, curves and the Preset member each ID
// carries. Drives PatchDumpV2 encode/decode and FastParamUpdate scaling. Global IDs (master volume,
// metronome, key/scale) are not in the table: they are performance state, not part of a patch.
enum class ParamCurve : uint8_t { Linear, Log, Enum, Bool };

struct ParamDef {
    ParamId    id;
    ParamCurve curve;
    float      lo;  // Enum: 0
    float      hi;  // Enum: highest index
    float (*get)(const Preset&);
    void  (*set)(Preset&, float);
};


#pragma pack(push, 1)
struct RawPatchData {
    char     name[12];          // [0..11] ASCII patch name, null-terminated
    uint8_t  model;             // [12] SynthModel index at the time of writing; 15+ (v1-era user models) reads as User
    uint8_t  waveform;          // [13] Waveform (0=Saw, 1=Square, 2=Triangle)
    uint8_t  voiceMode;         // [14] VoiceMode (0=Poly, 1=Mono)
    uint8_t  flags;             // [15] bit 0: chorusEnabled, bit 1: portamentoEnabled, bit 2: legatoEnabled

    // Core normalized parameters (16-bit unsigned, 0..65535)
    uint16_t cutoffHz;          // [16..17] Log-scaled 20 Hz .. 20000 Hz
    uint16_t resonanceQ;        // [18..19] Linear 0.5 .. 25.0
    uint16_t subOscLevel;       // [20..21] Linear 0.0 .. 1.0
    uint16_t detuneCents;       // [22..23] Linear 0.0 .. 50.0 cents
    uint16_t drive;             // [24..25] Linear 1.0 .. 3.5
    uint16_t filterEnvAmount;   // [26..27] Linear 0.0 .. 1.0

    // Amp ADSR (16-bit unsigned, 0..65535)
    uint16_t ampAttackMs;       // [28..29] Log-scaled 1.0 ms .. 4000.0 ms
    uint16_t ampDecayMs;        // [30..31] Log-scaled 5.0 ms .. 5000.0 ms
    uint16_t ampSustain;        // [32..33] Linear 0.0 .. 1.0
    uint16_t ampReleaseMs;      // [34..35] Log-scaled 5.0 ms .. 5000.0 ms

    // Filter ADSR (16-bit unsigned, 0..65535)
    uint16_t filterAttackMs;    // [36..37] Log-scaled 1.0 ms .. 4000.0 ms
    uint16_t filterDecayMs;     // [38..39] Log-scaled 5.0 ms .. 5000.0 ms
    uint16_t filterSustain;     // [40..41] Linear 0.0 .. 1.0
    uint16_t filterReleaseMs;   // [42..43] Log-scaled 5.0 ms .. 5000.0 ms

    // Secondary & Effects (8-bit unsigned, 0..255)
    uint8_t  chorusRate;        // [44] 0.1 .. 5.0 Hz
    uint8_t  chorusDepth;       // [45] 0.5 .. 5.0 ms
    uint8_t  chorusMix;         // [46] 0.0 .. 1.0
    uint8_t  glideTime;         // [47] 0.0 .. 1000.0 ms
};
#pragma pack(pop)
static_assert(sizeof(RawPatchData) == 48, "RawPatchData must be exactly 48 bytes");

// Legacy v1 (command 0x02) patch format. Still decoded from senders and old NVS blobs; no longer encoded
// by the firmware, which sends and stores PatchDumpV2.
constexpr size_t RAW_PATCH_SIZE    = sizeof(RawPatchData); // 48 bytes
constexpr size_t PACKED_PATCH_SIZE = 55;                   // 48 bytes 7-in-8 packed
constexpr size_t FULL_DUMP_PACKET_SIZE = 1 + 1 + 1 + 1 + 1 + PACKED_PATCH_SIZE + 1 + 1; // 62 bytes

// PatchDumpV2: F0 7D 01 05 <slot> <nameLen> <name...> {<id> <msb> <lsb>}... <xor> F7
// The "body" is everything from <nameLen> to the last entry byte; it is also the NVS blob format.
constexpr size_t MAX_SYSEX_MESSAGE_SIZE = 512;
constexpr size_t PATCH_NAME_MAX         = 11;
constexpr size_t PATCH_V2_FRAMING_SIZE  = 7; // F0 7D 01 05 <slot> ... <xor> F7
constexpr size_t MAX_PATCH_BODY_SIZE    = MAX_SYSEX_MESSAGE_SIZE - PATCH_V2_FRAMING_SIZE;
constexpr uint8_t PARAM_ID_EXTENDED     = 0x7F; // 0x7F <idHi> <idLo> <msb> <lsb>; no extended IDs defined yet

struct FastParamUpdatePayload {
    ParamId  paramId{ParamId::Cutoff};
    uint16_t value14{0}; // 14-bit normalized value [0..16383]
    uint8_t  slot{0};    // 0x00 = Active RAM, 24..32 = User Slots
    uint8_t  flags{0};
};

struct DecodedSysEx {
    SysExCommand    type{SysExCommand::FastParamUpdate};
    bool            isValid{false};
    HandshakeStatus errorStatus{HandshakeStatus::AckSuccess};

    // FastParamUpdate payload
    FastParamUpdatePayload fastParam;

    // PatchDump (v1) and PatchDumpV2 payloads
    uint8_t      slot{0};
    RawPatchData patchData{};             // v1
    std::vector<uint8_t> patchBody;       // v2 body, already validated

    // SaveSlot / RequestDump payload
    uint8_t      targetSlot{0};

    // Handshake payload
    SysExCommand    refCommand{SysExCommand::FastParamUpdate};
    HandshakeStatus handshakeStatus{HandshakeStatus::AckSuccess};
};

class SysExProtocol {
public:
    // 7-in-8 Bit Packing & Unpacking
    // Converts raw 8-bit bytes to 7-bit safe MIDI bytes (MSB = 0)
    static size_t pack7Bit(const uint8_t* inRaw, size_t inLen, uint8_t* outMidi, size_t maxOutLen);

    // Converts 7-bit safe MIDI bytes back into raw 8-bit bytes
    static size_t unpack7Bit(const uint8_t* inMidi, size_t inLen, uint8_t* outRaw, size_t maxOutLen);

    // Compute 7-bit XOR checksum over a byte sequence
    static uint8_t computeChecksum(const uint8_t* data, size_t len);

    // Packet Encoders
    // Encodes Command 0x01: Fast Single Parameter Update (10 bytes total)
    static size_t encodeFastParamUpdate(ParamId param, uint16_t val14, uint8_t slot, uint8_t* outBuffer, size_t maxLen);

    // Encodes Command 0x02: Full Patch Dump (62 bytes total)
    static size_t encodePatchDump(uint8_t slot, const RawPatchData& patch, uint8_t* outBuffer, size_t maxLen);

    // Encodes Command 0x03: Save Active Patch to a User Slot 24..32 (6 bytes total)
    static size_t encodeSaveUserSlot(uint8_t slot, uint8_t* outBuffer, size_t maxLen);

    // Encodes Command 0x04: Request Patch Dump from Slot (6 bytes total)
    static size_t encodeRequestPatchDump(uint8_t slot, uint8_t* outBuffer, size_t maxLen);

    // Encodes Command 0x0F: ACK / NACK Handshake Response (7 bytes total)
    static size_t encodeHandshake(SysExCommand refCmd, HandshakeStatus status, uint8_t* outBuffer, size_t maxLen);

    // Packet Decoder
    static DecodedSysEx decodeSysEx(const uint8_t* data, size_t len);

    // Helpers to convert between Preset struct and RawPatchData (legacy v1)
    static void presetToRawPatch(const Preset& preset, RawPatchData& outRaw);
    static void rawPatchToPreset(const RawPatchData& raw, Preset& outPreset, char* nameStorage = nullptr);

    // Patch-scope parameter table
    static const ParamDef* patchParams(size_t& count);
    static const ParamDef* findPatchParam(ParamId id);
    static uint16_t paramToValue14(const ParamDef& def, float value);
    // Enums and booleans take the raw index. As FastParamUpdate always accepted: on/off values (booleans,
    // VoiceMode, EngineMode) read any nonzero value as on, and larger enum values beyond the range are read
    // as normalized (14-bit full scale)
    static float value14ToParam(const ParamDef& def, uint16_t value14);

    // PatchDumpV2 (ADR-0006)
    static size_t encodePatchBody(const Preset& preset, uint8_t* outBody, size_t maxLen);
    static bool   isValidPatchBody(const uint8_t* body, size_t len);
    // Applies the body's entries on top of outPreset (callers start from the defaults); unknown IDs
    // are skipped, duplicates resolve to the last one. Returns false (outPreset untouched) if malformed.
    static bool   decodePatchBody(const uint8_t* body, size_t len, Preset& outPreset, char* nameStorage);
    static size_t encodePatchDumpV2(uint8_t slot, const Preset& preset, uint8_t* outBuffer, size_t maxLen);
};

} // namespace Synth
