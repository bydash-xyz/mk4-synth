#pragma once

#include "Parameters.h"
#include "Filter.h"
#include <algorithm>
#include <array>
#include <string>

namespace Synth {

enum class SynthModel : uint8_t {
    // Factory Models: Subtractive Analog (Slots #1..#6)
    Juno60            = 0,  // Slot #1
    Minimoog          = 1,  // Slot #2
    Prophet5          = 2,  // Slot #3
    TB303             = 3,  // Slot #4
    CS80              = 4,  // Slot #5
    OBXa              = 5,  // Slot #6
    // Factory Models: Casio PT-10 Lo-Fi Digital (Slots #7..#10)
    CasioPT10_Piano   = 6,  // Slot #7: Casio PT-10 Piano
    CasioPT10_Fantasy = 7,  // Slot #8: Casio PT-10 Fantasy
    CasioPT10_Violin  = 8,  // Slot #9: Casio PT-10 Violin
    CasioPT10_Flute   = 9,  // Slot #10: Casio PT-10 Flute
    // Factory Models: 2-Operator Polyphonic FM Keyboard (Slots #11..#15)
    DX_EPiano         = 10, // Slot #11: Yamaha DX7 Classic Electric Piano
    FM_CrystalTines   = 11, // Slot #12: FM Crystal Tines
    FM_WarmRhodes     = 12, // Slot #13: FM Warm Rhodes / Suitcase Tremolo
    FM_Clavinet       = 13, // Slot #14: FM Clavinet
    FM_DrawbarOrgan   = 14, // Slot #15: FM Drawbar Organ
    // Factory Models: PPG / Waldorf Wavetable (Slots #16..#17)
    PPG_Wave          = 15, // Slot #16: PPG Wave 2.3 style digital wavetable sweep
    WT_VoxPad         = 16, // Slot #17: Waldorf Microwave style formant wavetable pad
    // Factory Models: Casio CZ Phase Distortion (Slots #18..#19)
    CZ_Bass           = 17, // Slot #18: Casio CZ-101 DCW-swept bass
    CZ_Reso           = 18, // Slot #19: Casio CZ-101 resonance lead (two detuned lines)
    // Factory Models: Karplus-Strong Plucked String (Slots #20..#21)
    KS_Guitar         = 19, // Slot #20: Plucked steel-string guitar
    KS_Koto           = 20, // Slot #21: Bright koto / harp pluck
    // Factory Models: Additive Hammond Drawbar Organ (Slots #22..#23)
    B3_Jazz           = 21, // Slot #22: Hammond B-3 888000000, third-harmonic percussion
    B3_Gospel         = 22, // Slot #23: Hammond B-3 888808008 with fast Leslie
    // Factory Models: Group 2 Subtractive Expansions (Slots #24..#29)
    Odyssey           = 23, // Slot #24: ARP Odyssey duophonic lead (hard sync saw + ring mod)
    VintageJup        = 24, // Slot #25: Roland Jupiter-8 cross-modulated poly synth
    TubeSynth         = 25, // Slot #26: Tube-saturated poly synth (asymmetric triode)
    SubFactory        = 26, // Slot #27: 808 sub bass (click+body pitch envelope + tube drive)
    Memorymoog        = 27, // Slot #28: 3-oscillator detuned ladder poly brass
    Iona              = 28, // Slot #29: SH-101 acid/electro mono bass
    // Factory Models: Group 3 New DSP Engines (Slots #30..#31)
    OPx4              = 29, // Slot #30: 4-operator FM synth (8 algorithms)
    Solina            = 30, // Slot #31: ARP Solina String Ensemble (formants + 3-phase chorus)
    // Factory Models: Group 4 Caricature Engines (Slots #32..#36)
    StagePiano        = 31, // Slot #32: Commuted waveguide acoustic grand piano
    StudioStrings     = 32, // Slot #33: Bowed strings with rosin noise + violin body
    FabricXl          = 33, // Slot #34: Dual wavetable + chime & breath pad
    Mariana           = 34, // Slot #35: Dual-path sub + tube mid bass
    Hype              = 35, // Slot #36: Hypersaw + comb stereoizer + pump envelope
    // User Customizable Slots (Slots #37..#45)
    User_01           = 36, // Slot #37 (Default User Slot)
    User_02           = 37, // Slot #38
    User_03           = 38, // Slot #39
    User_04           = 39, // Slot #40
    User_05           = 40, // Slot #41
    User_06           = 41, // Slot #42
    User_07           = 42, // Slot #43
    User_08           = 43, // Slot #44
    User_09           = 44, // Slot #45
    Count             = 45,
    User              = User_01, // Backwards-compatible alias
    CasioPT10         = CasioPT10_Piano // Backwards-compatible alias
};

constexpr size_t FACTORY_PRESET_COUNT = 36;
constexpr size_t USER_PRESET_COUNT    = 9;
constexpr size_t TOTAL_PRESET_COUNT   = 45;

constexpr uint8_t FACTORY_SLOT_START  = 1;
constexpr uint8_t FACTORY_SLOT_END    = 36;
constexpr uint8_t USER_SLOT_START     = 37;
constexpr uint8_t USER_SLOT_END       = 45;
constexpr uint8_t TOTAL_SLOT_COUNT    = 45;
constexpr uint8_t USER_PRESET_SLOT    = USER_SLOT_START; // Slot #37 (Default User Slot)

inline bool isUserSlot(uint8_t slot) {
    return slot >= USER_SLOT_START && slot <= USER_SLOT_END;
}

inline bool isFactorySlot(uint8_t slot) {
    return slot >= FACTORY_SLOT_START && slot <= FACTORY_SLOT_END;
}

inline bool isCasioModel(SynthModel m) {
    return m == SynthModel::CasioPT10_Piano ||
           m == SynthModel::CasioPT10_Fantasy ||
           m == SynthModel::CasioPT10_Violin ||
           m == SynthModel::CasioPT10_Flute;
}

inline bool isFmModel(SynthModel m) {
    return m == SynthModel::DX_EPiano ||
           m == SynthModel::FM_CrystalTines ||
           m == SynthModel::FM_WarmRhodes ||
           m == SynthModel::FM_Clavinet ||
           m == SynthModel::FM_DrawbarOrgan;
}

inline bool isUserModel(SynthModel m) {
    return static_cast<uint8_t>(m) >= static_cast<uint8_t>(SynthModel::User_01) &&
           static_cast<uint8_t>(m) <= static_cast<uint8_t>(SynthModel::User_09);
}

inline bool isFactoryModel(SynthModel m) {
    return static_cast<uint8_t>(m) < FACTORY_PRESET_COUNT;
}


enum class CasioTone : uint8_t {
    Piano   = 0,
    Fantasy = 1,
    Violin  = 2,
    Flute   = 3,
    Count   = 4
};

struct KnobBank {
    const char* labels[8];
    uint8_t     ccs[8];
    uint8_t     defaults[8]; // Default MIDI CC values [0..127]
};

struct Preset {
    SynthModel model;
    const char* name;          // Max 16 chars for MPK display, e.g. "JUNO-60"
    const char* description;   // Short explanation
    Waveform waveform;
    float subOscLevel;         // 0.0 to 1.0
    float detuneCents;         // 0 to 50 cents
    float drive;               // 1.0 = clean, >1.0 = warm saturated
    float cutoffHz;
    float resonanceQ;
    float filterEnvAmount;

    // Envelopes
    float ampAttackMs;
    float ampDecayMs;
    float ampSustain;
    float ampReleaseMs;

    float filterAttackMs;
    float filterDecayMs;
    float filterSustain;
    float filterReleaseMs;

    // Chorus
    bool  chorusEnabled;
    float chorusRateHz;
    float chorusDepthMs;
    float chorusMix;

    // Bank A (Primary Knobs 1..8) and Bank B (Shift Knobs 9..16)
    KnobBank bankA;
    KnobBank bankB;

    // Voice Mode & Glide
    VoiceMode voiceMode{VoiceMode::Polyphonic};
    bool      portamentoEnabled{false};
    bool      legatoEnabled{false};
    float     glideTimeMs{0.0f};

    // FM Synthesis & Modulation Parameters
    EngineMode engineMode{EngineMode::Subtractive};
    float      modRatio{1.0f};
    float      modDepth{2.0f};
    float      modFeedback{0.0f};
    float      tremoloDepth{0.0f};
    float      tremoloRateHz{4.5f};

    // Instrument character (filter topology, key tracking, velocity response...) taken from this
    // factory model. Count = derive from `model` (factory presets) or use the generic user voicing.
    SynthModel voicing{SynthModel::Count};

    // Wavetable, phase distortion, pluck and additive engine parameters
    TimbreParams timbre{};
};

// Synthesis engine a preset plays on. FM factory presets predate the engineMode field being sent in
// legacy dumps, so their model decides; everything else carries its engine in engineMode.
inline EngineMode presetEngineMode(const Preset& p) {
    if (p.engineMode == EngineMode::StageEp) return EngineMode::StageEp;
    if (isFmModel(p.model)) return EngineMode::FM;
    return static_cast<uint8_t>(p.engineMode) < static_cast<uint8_t>(EngineMode::Count) ? p.engineMode
                                                                                        : EngineMode::Subtractive;
}

using SynthParameters = Preset;

enum ArchetypeId : uint8_t {
    ARCH_MINI_D   = 0,
    ARCH_JURA     = 1,
    ARCH_BASSLINE = 2,
    ARCH_ORGAN    = 3,
    ARCH_ELECTRIC = 4,
    ARCH_COUNT    = 5
};

struct engine_calibration_t {
    uint8_t archetype_id;
    uint8_t max_voices;
    float voice_norm_scalar;
    float resonance_makeup;
    float master_trim_linear;
};

static constexpr engine_calibration_t kArchetypeCalib[ARCH_COUNT] = {
    { ARCH_MINI_D,   4, 0.500f, 1.45f, 0.891f },
    { ARCH_JURA,     6, 0.408f, 1.25f, 0.794f },
    { ARCH_BASSLINE, 1, 1.000f, 1.60f, 0.707f },
    { ARCH_ORGAN,    8, 0.250f, 1.00f, 0.630f },
    { ARCH_ELECTRIC, 8, 0.353f, 1.10f, 1.122f }
};

// Hardware character of each modelled instrument, independent of the patch parameters
struct ModelVoicing {
    FilterModel filter;     // Filter topology
    float   keyTrack;       // Filter keyboard tracking (0 = off, 1 = cutoff follows pitch 1:1)
    float   velToAmp;       // Velocity -> loudness (0 = no velocity response, as on most vintage analogs)
    bool    accent;         // TB-303 style accent on velocity >= 100
    bool    oscKeySync;     // Restart oscillator phase on notes started from silence (digital/FM)
    float   analogDrift;    // Per-note pitch / cutoff variation (0 = digital-stable)
    uint8_t voices;         // Original polyphony (0 = engine maximum)
    SubModMode subMod{SubModMode::None};
};

inline ModelVoicing getModelVoicing(SynthModel model) {
    switch (model) {
        //                                  filter                 keyTrk vel   accent sync   drift voices subMod
        case SynthModel::Juno60:   return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 0.3f, 6, SubModMode::None}; // DCOs: stable pitch
        case SynthModel::Minimoog: return {FilterModel::Ladder24, 0.33f, 0.0f, false, false, 1.0f, 4, SubModMode::None};
        case SynthModel::Prophet5: return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 0.8f, 5, SubModMode::None}; // CEM3320 4-pole
        case SynthModel::TB303:    return {FilterModel::Ladder24, 0.0f,  0.0f, true,  false, 0.3f, 1, SubModMode::None};
        case SynthModel::CS80:     return {FilterModel::Svf12,    0.5f,  0.7f, false, false, 0.8f, 8, SubModMode::None};
        case SynthModel::OBXa:     return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 0.8f, 8, SubModMode::None};
        case SynthModel::CasioPT10_Piano:
        case SynthModel::CasioPT10_Fantasy:
        case SynthModel::CasioPT10_Violin:
        case SynthModel::CasioPT10_Flute:
                                   // The real PT-10 is monophonic: the presets default to Mono (last-note priority).
                                   // No voice cap here, so the PolyMode knob (CC 92) can open up the engine maximum.
                                   return {FilterModel::Svf12,    0.0f,  0.0f, false, true,  0.0f, 0, SubModMode::None};
        case SynthModel::DX_EPiano:
        case SynthModel::FM_CrystalTines:
        case SynthModel::FM_WarmRhodes:
        case SynthModel::FM_Clavinet:
        case SynthModel::FM_DrawbarOrgan:
                                   return {FilterModel::Svf12,    0.0f,  1.0f, false, true,  0.0f, 8, SubModMode::None};
        // PPG Wave 2.3: digital oscillators into an SSM2044 4-pole VCF; Microwave: CEM3387 4-pole
        case SynthModel::PPG_Wave: return {FilterModel::Ladder24, 0.5f,  0.5f, false, true,  0.0f, 8, SubModMode::None};
        case SynthModel::WT_VoxPad:return {FilterModel::Ladder24, 0.5f,  0.6f, false, true,  0.0f, 8, SubModMode::None};
        // CZ-101: no filter, no velocity; 8 voices with one line per note, 4 with two (CZ RESO)
        case SynthModel::CZ_Bass:  return {FilterModel::Svf12,    0.0f,  0.0f, false, true,  0.0f, 8, SubModMode::None};
        case SynthModel::CZ_Reso:  return {FilterModel::Svf12,    0.0f,  0.0f, false, true,  0.0f, 4, SubModMode::None};
        // Plucked strings: velocity sets pick strength (brightness and level)
        case SynthModel::KS_Guitar:
        case SynthModel::KS_Koto:  return {FilterModel::Svf12,    0.0f,  1.0f, false, true,  0.0f, 0, SubModMode::None};
        // Tonewheel organ: no velocity, stable pitch
        case SynthModel::B3_Jazz:
        case SynthModel::B3_Gospel:return {FilterModel::Svf12,    0.0f,  0.0f, false, true,  0.0f, 8, SubModMode::None};
        case SynthModel::Odyssey:       return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 0.6f, 1, SubModMode::HardSyncRing};
        case SynthModel::VintageJup:    return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 0.5f, 8, SubModMode::CrossMod};
        case SynthModel::TubeSynth:     return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 0.5f, 8, SubModMode::TubeDrive};
        case SynthModel::SubFactory:    return {FilterModel::Ladder24, 0.0f,  0.0f, false, true,  0.0f, 1, SubModMode::Sub808};
        case SynthModel::Memorymoog:    return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 1.2f, 6, SubModMode::None};
        case SynthModel::Iona:          return {FilterModel::Ladder24, 0.5f,  0.0f, false, false, 0.2f, 1, SubModMode::None};
        case SynthModel::OPx4:          return {FilterModel::Svf12,    0.0f,  1.0f, false, true,  0.0f, 8, SubModMode::None};
        case SynthModel::Solina:        return {FilterModel::Svf12,    0.0f,  0.0f, false, true,  0.0f, 8, SubModMode::None};
        case SynthModel::StagePiano:    return {FilterModel::Svf12,    0.0f,  1.0f, false, true,  0.0f, 8, SubModMode::None};
        case SynthModel::StudioStrings: return {FilterModel::Svf12,    0.5f,  0.7f, false, false, 0.3f, 8, SubModMode::None};
        case SynthModel::FabricXl:      return {FilterModel::Ladder24, 0.5f,  0.7f, false, true,  0.0f, 8, SubModMode::None};
        case SynthModel::Mariana:       return {FilterModel::Ladder24, 0.3f,  0.0f, false, true,  0.1f, 1, SubModMode::None};
        case SynthModel::Hype:          return {FilterModel::Ladder24, 0.5f,  0.8f, false, false, 0.4f, 8, SubModMode::None};
        default:                   // Generic user patch
                                   return {FilterModel::Ladder24, 0.5f,  0.5f, false, false, 0.5f, 0, SubModMode::None};
    }
}

// Voicing a generic patch (no factory voicing) takes on each engine (Count = generic subtractive)
inline SynthModel defaultVoicingForEngine(EngineMode mode) {
    switch (mode) {
        case EngineMode::FM:            return SynthModel::DX_EPiano;
        case EngineMode::Wavetable:     return SynthModel::PPG_Wave;
        case EngineMode::PhaseDist:     return SynthModel::CZ_Bass;
        case EngineMode::Pluck:         return SynthModel::KS_Guitar;
        case EngineMode::Additive:      return SynthModel::B3_Jazz;
        case EngineMode::Opx4:          return SynthModel::OPx4;
        case EngineMode::StagePiano:    return SynthModel::StagePiano;
        case EngineMode::StageEp:       return SynthModel::FM_WarmRhodes;
        case EngineMode::StudioStrings: return SynthModel::StudioStrings;
        case EngineMode::FabricXl:      return SynthModel::FabricXl;
        case EngineMode::Mariana:       return SynthModel::Mariana;
        case EngineMode::Hype:          return SynthModel::Hype;
        default:                        return SynthModel::Count;
    }
}

// Which factory model's voicing a preset uses (Count = generic user voicing)
inline SynthModel resolveVoicingModel(const Preset& p) {
    if (isFactoryModel(p.voicing)) return p.voicing;
    if (isFactoryModel(p.model)) return p.model;
    return defaultVoicingForEngine(presetEngineMode(p));
}

// Output level trim per factory model, in dB. The models differ by over 10 dB in raw loudness
// (single-oscillator Casio tones vs. filtered analog bass, velocity-sensitive FM), so without it
// switching presets jumps the volume. Measured with synth_render playing PresetLoudnessPhrase (in
// CMakeLists.txt: chords, then a single-note line, as the MK4 is usually played) at velocity 110, and
// trimmed so each model's BS.1770 integrated loudness is -17 LUFS. Mono models play one note of each
// chord, as they would on stage. Re-measure after changing a preset's sound; PresetLoudnessTest guards
// the spread.
inline float getModelLevelDb(SynthModel model) {
    switch (model) {
        case SynthModel::Juno60:            return   0.6f;
        case SynthModel::Minimoog:          return   7.8f;
        case SynthModel::Prophet5:          return   0.1f;
        case SynthModel::TB303:             return  -0.5f;
        case SynthModel::CS80:              return  -1.6f;
        case SynthModel::OBXa:              return   2.4f;
        case SynthModel::CasioPT10_Piano:   return   2.2f;
        case SynthModel::CasioPT10_Fantasy: return  -4.1f;
        case SynthModel::CasioPT10_Violin:  return  -3.0f;
        case SynthModel::CasioPT10_Flute:   return  -3.4f;
        case SynthModel::DX_EPiano:         return  -3.9f;
        case SynthModel::FM_CrystalTines:   return  -5.9f;
        case SynthModel::FM_WarmRhodes:     return   3.4f;
        case SynthModel::FM_Clavinet:       return   0.1f;
        case SynthModel::FM_DrawbarOrgan:   return  -2.2f;
        case SynthModel::PPG_Wave:          return   5.6f;
        case SynthModel::WT_VoxPad:         return   4.4f;
        case SynthModel::CZ_Bass:           return  -5.1f;
        case SynthModel::CZ_Reso:           return  -2.9f;
        case SynthModel::KS_Guitar:         return   4.7f;
        case SynthModel::KS_Koto:           return   5.6f;
        case SynthModel::B3_Jazz:           return  -0.7f;
        case SynthModel::B3_Gospel:         return  -0.7f;
        case SynthModel::Odyssey:           return   2.9f;
        case SynthModel::VintageJup:        return   1.1f;
        case SynthModel::TubeSynth:         return  -3.1f;
        case SynthModel::SubFactory:        return   2.7f;
        case SynthModel::Memorymoog:        return  -0.2f;
        case SynthModel::Iona:              return   6.9f;
        case SynthModel::OPx4:              return  -4.3f;
        case SynthModel::Solina:            return  -2.4f;
        case SynthModel::StagePiano:        return   3.7f;
        case SynthModel::StudioStrings:     return  -2.1f;
        case SynthModel::FabricXl:          return   3.1f;
        case SynthModel::Mariana:           return  -6.1f;
        case SynthModel::Hype:              return   7.0f;
        default:                            return  0.0f; // Generic user patch
    }
}

inline Preset makeDefaultUserPreset(SynthModel model, const char* name, const char* description) {
    return {
        .model = model, .name = name, .description = description,
        .waveform = Waveform::Sawtooth, .subOscLevel = 0.25f, .detuneCents = 8.0f, .drive = 1.0f,
        .cutoffHz = 2000.0f, .resonanceQ = 2.0f, .filterEnvAmount = 0.5f,
        .ampAttackMs = 10.0f, .ampDecayMs = 300.0f, .ampSustain = 0.7f, .ampReleaseMs = 350.0f,
        .filterAttackMs = 20.0f, .filterDecayMs = 350.0f, .filterSustain = 0.3f, .filterReleaseMs = 300.0f,
        .chorusEnabled = false, .chorusRateHz = 0.5f, .chorusDepthMs = 2.5f, .chorusMix = 0.5f,
        .bankA = {
            .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
            .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
            .defaults = {85, 35, 32, 0, 35, 75, 89, 78},
        },
        .bankB = {
            .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ChoRate", "ChoDepth", "Master"},
            .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
            .defaults = {64, 46, 78, 38, 75, 10, 56, 100},
        },
    };
}

// Starting point of the wavetable / phase distortion / pluck / additive factory presets
inline Preset makeEnginePreset(SynthModel model, const char* name, const char* description, EngineMode mode,
                               const KnobBank& bankA, const KnobBank& bankB) {
    Preset p = makeDefaultUserPreset(model, name, description);
    p.engineMode = mode;
    p.bankA = bankA;
    p.bankB = bankB;
    return p;
}

inline const std::array<Preset, static_cast<size_t>(SynthModel::Count)>& getPresets() {
    static const std::array<Preset, static_cast<size_t>(SynthModel::Count)> presets = {{
        // 0. Roland Juno-60
        {
            .model = SynthModel::Juno60,
            .name = "JUNO-60",
            .description = "Roland Juno-60 Lush Chorus Pad & Punchy Bass",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.5f,
            .detuneCents = 6.0f, // 6 cents gentle beating
            .drive = 1.0f, // Clean
            .cutoffHz = 1400.0f,
            .resonanceQ = 2.5f, // Classic Roland resonance
            .filterEnvAmount = 0.45f,
            .ampAttackMs = 10.0f, .ampDecayMs = 350.0f, .ampSustain = 0.65f, .ampReleaseMs = 450.0f,
            .filterAttackMs = 25.0f, .filterDecayMs = 400.0f, .filterSustain = 0.30f, .filterReleaseMs = 400.0f,
            .chorusEnabled = true, .chorusRateHz = 0.75f, .chorusDepthMs = 3.0f, .chorusMix = 0.60f, // Stereo BBD Chorus ON!
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {78, 41, 64, 76, 35, 78, 83, 83},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {57, 49, 81, 38, 81, 17, 71, 100},
            },
        },

        // 1. Moog Minimoog Model D
        {
            .model = SynthModel::Minimoog,
            .name = "MINIMOOG",
            .description = "Moog Model D Fat 3-Osc Ladder Bass & Screaming Lead",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.6f, // Sub-osc for thick low-end
            .detuneCents = 14.0f, // Fat detune
            .drive = 1.9f, // Heavy analog transistor overdrive drive!
            .cutoffHz = 750.0f,
            .resonanceQ = 4.2f, // Screaming Moog resonance
            .filterEnvAmount = 0.75f, // Punchy filter envelope
            .ampAttackMs = 1.5f, .ampDecayMs = 220.0f, .ampSustain = 0.25f, .ampReleaseMs = 120.0f, // Snappy bass envelope
            .filterAttackMs = 5.0f, .filterDecayMs = 280.0f, .filterSustain = 0.10f, .filterReleaseMs = 100.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f, // Chorus OFF (pure raw analog grit)
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "Drive", "Detune", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 87, 94, 73, 75, 79, 72},
                .defaults = {66, 55, 46, 36, 6,  70, 32, 58},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ModRate", "ModDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 89, 90, 7},
                .defaults = {95, 24, 74, 13, 55, 50, 0, 100},
            },
            .voiceMode = VoiceMode::Monophonic,
            .portamentoEnabled = true,
            .legatoEnabled = true,
            .glideTimeMs = 15.0f,
        },

        // 2. Sequential Circuits Prophet-5
        {
            .model = SynthModel::Prophet5,
            .name = "PROPHET-5",
            .description = "Sequential Circuits Prophet-5 Polyphonic Brass & Warmth",
            .waveform = Waveform::Square, // Pulse wave
            .subOscLevel = 0.0f, // No sub
            .detuneCents = 9.0f, // Polyphonic beating
            .drive = 1.1f, // Slight warmth
            .cutoffHz = 2400.0f,
            .resonanceQ = 1.8f,
            .filterEnvAmount = 0.60f,
            .ampAttackMs = 20.0f, .ampDecayMs = 500.0f, .ampSustain = 0.50f, .ampReleaseMs = 450.0f,
            .filterAttackMs = 30.0f, .filterDecayMs = 450.0f, .filterSustain = 0.25f, .filterReleaseMs = 350.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "Drive", "Detune", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 87, 94, 73, 75, 79, 72},
                .defaults = {88, 33, 5,  23, 45, 85, 64, 83},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ModRate", "Wavefrm", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 89, 70, 7},
                .defaults = {76, 52, 83, 32, 78, 40, 50, 100},
            },
        },

        // 3. Roland TB-303
        {
            .model = SynthModel::TB303,
            .name = "TB-303 ACID",
            .description = "Roland TB-303 Acid Techno Squelch & High Resonance",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.0f,
            .detuneCents = 0.0f, // Pure single oscillator
            .drive = 1.6f, // Acid distortion
            .cutoffHz = 500.0f,
            .resonanceQ = 14.0f, // Extreme resonance self-oscillation!
            .filterEnvAmount = 0.95f, // Huge filter modulation sweep
            .ampAttackMs = 3.0f, .ampDecayMs = 1200.0f, .ampSustain = 0.0f, .ampReleaseMs = 25.0f, // VCA: gate-held with a long fixed decay, quick gate-off
            .filterAttackMs = 1.0f, .filterDecayMs = 140.0f, .filterSustain = 0.0f, .filterReleaseMs = 60.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "Accent", "Distort", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 88, 87, 73, 75, 79, 72},
                .defaults = {59, 106, 90, 30, 17, 101, 0,  30},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Slide", "Wavefrm", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 65, 70, 7},
                .defaults = {121, 0,  61, 0,  46, 64, 20, 100},
            },
            .voiceMode = VoiceMode::Monophonic,
            .portamentoEnabled = true,
            .legatoEnabled = true,
            .glideTimeMs = 60.0f,
        },

        // 4. Yamaha CS-80
        {
            .model = SynthModel::CS80,
            .name = "CS-80 BLADE",
            .description = "Yamaha CS-80 Vangelis Blade Runner Swelling Brass Lead",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.2f,
            .detuneCents = 16.0f, // Slow dramatic chorusing beat
            .drive = 1.1f,
            .cutoffHz = 1600.0f,
            .resonanceQ = 2.2f,
            .filterEnvAmount = 0.40f,
            .ampAttackMs = 350.0f, .ampDecayMs = 900.0f, .ampSustain = 0.80f, .ampReleaseMs = 1200.0f, // Dramatic slow swell attack
            .filterAttackMs = 400.0f, .filterDecayMs = 800.0f, .filterSustain = 0.40f, .filterReleaseMs = 1000.0f,
            .chorusEnabled = true, .chorusRateHz = 0.5f, .chorusDepthMs = 2.8f, .chorusMix = 0.50f, // Rich expansive chorus
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Detune", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 94, 73, 75, 79, 72},
                .defaults = {80, 37, 25, 41, 90, 95, 102, 101},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "VibRate", "VibDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 89, 90, 7},
                .defaults = {51, 92, 93, 51, 97, 45, 0, 100},
            },
        },

        // 5. Oberheim OB-Xa
        {
            .model = SynthModel::OBXa,
            .name = "OB-XA BRASS",
            .description = "Oberheim OB-Xa 80s Huge Punchy Synth Brass ('Jump')",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.1f,
            .detuneCents = 11.0f, // Huge wide detune
            .drive = 1.2f,
            .cutoffHz = 2800.0f,
            .resonanceQ = 1.6f,
            .filterEnvAmount = 0.65f,
            .ampAttackMs = 15.0f, .ampDecayMs = 400.0f, .ampSustain = 0.55f, .ampReleaseMs = 350.0f,
            .filterAttackMs = 20.0f, .filterDecayMs = 350.0f, .filterSustain = 0.20f, .filterReleaseMs = 300.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "Drive", "Detune", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 87, 94, 73, 75, 79, 72},
                .defaults = {91, 30, 10, 28, 41, 81, 70, 78},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ModRate", "Wavefrm", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 89, 70, 7},
                .defaults = {83, 45, 78, 25, 75, 13, 20, 100},
            },
        },

        // 6. Casio PT-10 Piano (Slot #7)
        {
            .model = SynthModel::CasioPT10_Piano,
            .name = "PT-10 PIANO",
            .description = "Casio PT-10 Consonant-Vowel Piano (Decaying Percussive Key Strike)",
            .waveform = Waveform::Square,
            .subOscLevel = 0.0f, // Sub-osc off
            .detuneCents = 0.0f, // Detune cents
            .drive = 1.10f, // Subtle lo-fi digital grit
            .cutoffHz = 3800.0f, // Snappy lo-fi RC reconstruction filter
            .resonanceQ = 0.8f, // Low resonance RC response
            .filterEnvAmount = 0.0f, // Static filter (no filter envelope)
            .ampAttackMs = 2.0f, .ampDecayMs = 450.0f, .ampSustain = 0.0f, .ampReleaseMs = 60.0f, // Amp ADSR: snappy 2ms click attack, 450ms decay, 0 sustain, 60ms release
            .filterAttackMs = 10.0f, .filterDecayMs = 100.0f, .filterSustain = 1.0f, .filterReleaseMs = 100.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f, // Mono dry output
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "VibRate", "Tone", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 89, 70, 73, 75, 79, 72},
                .defaults = {96, 15, 60, 10, 11,  83, 0,   46},
            },
            .bankB = {
                .labels   = {"Drive", "Glide", "PulseWid", "VibDepth", "Octave", "PolyMode", "Chorus", "Master"},
                .ccs      = {87, 93, 76, 90, 82, 92, 78, 7},
                .defaults = {5, 0, 92, 0, 0,  0,   0,  100},
            },
            .voiceMode = VoiceMode::Monophonic,
        },

        // 7. Casio PT-10 Fantasy (Slot #8)
        {
            .model = SynthModel::CasioPT10_Fantasy,
            .name = "PT-10 FNTSY",
            .description = "Casio PT-10 Fantasy Square + Octave Harmonic Lead",
            .waveform = Waveform::Square,
            .subOscLevel = 0.35f, // Octave harmonic overtone for signature Fantasy tone
            .detuneCents = 0.0f, // Single oscillator pitch
            .drive = 1.15f, // Subtle lo-fi digital saturation
            .cutoffHz = 4800.0f, // Warm analog reconstruction filter cutoff
            .resonanceQ = 0.8f, // Low resonance RC response
            .filterEnvAmount = 0.0f, // Static filter
            .ampAttackMs = 10.0f, .ampDecayMs = 350.0f, .ampSustain = 0.85f, .ampReleaseMs = 250.0f, // Amp ADSR: sustained lead
            .filterAttackMs = 10.0f, .filterDecayMs = 100.0f, .filterSustain = 1.0f, .filterReleaseMs = 100.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f, // Dry
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "VibRate", "Tone", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 89, 70, 73, 75, 79, 72},
                .defaults = {101, 15, 44, 48, 35, 78, 108, 72},
            },
            .bankB = {
                .labels   = {"Drive", "Glide", "PulseWid", "VibDepth", "Octave", "PolyMode", "Chorus", "Master"},
                .ccs      = {87, 93, 76, 90, 82, 92, 78, 7},
                .defaults = {8, 0, 64, 32, 45, 0,  0,  100},
            },
            .voiceMode = VoiceMode::Monophonic,
        },

        // 8. Casio PT-10 Violin (Slot #9)
        {
            .model = SynthModel::CasioPT10_Violin,
            .name = "PT-10 VIOLN",
            .description = "Casio PT-10 Multipulse Violin (Narrow Pulse Bowed String Swell)",
            .waveform = Waveform::Square,
            .subOscLevel = 0.0f, // Sub-osc off
            .detuneCents = 0.0f, // Single pitch
            .drive = 1.15f, // Grit
            .cutoffHz = 4200.0f, // Cutoff
            .resonanceQ = 0.9f, // Resonance
            .filterEnvAmount = 0.0f, // Static filter
            .ampAttackMs = 45.0f, .ampDecayMs = 200.0f, .ampSustain = 0.90f, .ampReleaseMs = 180.0f, // Amp ADSR: 45ms bowed swell
            .filterAttackMs = 10.0f, .filterDecayMs = 100.0f, .filterSustain = 1.0f, .filterReleaseMs = 100.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "VibRate", "Tone", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 89, 70, 73, 75, 79, 72},
                .defaults = {98, 18, 39, 80, 58, 68, 115, 66},
            },
            .bankB = {
                .labels   = {"Drive", "Glide", "PulseWid", "VibDepth", "Octave", "PolyMode", "Chorus", "Master"},
                .ccs      = {87, 93, 76, 90, 82, 92, 78, 7},
                .defaults = {8, 0, 24, 19, 0,  0,   0,  100},
            },
            .voiceMode = VoiceMode::Monophonic,
        },

        // 9. Casio PT-10 Flute (Slot #10)
        {
            .model = SynthModel::CasioPT10_Flute,
            .name = "PT-10 FLUTE",
            .description = "Casio PT-10 Mellow Filtered Square Flute (Smooth Breath Attack)",
            .waveform = Waveform::Square,
            .subOscLevel = 0.0f,
            .detuneCents = 0.0f,
            .drive = 1.05f,
            .cutoffHz = 1400.0f, // Mellow low-pass
            .resonanceQ = 0.7f,
            .filterEnvAmount = 0.0f,
            .ampAttackMs = 30.0f, .ampDecayMs = 200.0f, .ampSustain = 0.90f, .ampReleaseMs = 150.0f, // Amp ADSR: 30ms breath attack
            .filterAttackMs = 10.0f, .filterDecayMs = 100.0f, .filterSustain = 1.0f, .filterReleaseMs = 100.0f,
            .chorusEnabled = false, .chorusRateHz = 0.0f, .chorusDepthMs = 0.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "VibRate", "Tone", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 89, 70, 73, 75, 79, 72},
                .defaults = {78, 12, 41, 110, 52, 68, 115, 63},
            },
            .bankB = {
                .labels   = {"Drive", "Glide", "PulseWid", "VibDepth", "Octave", "PolyMode", "Chorus", "Master"},
                .ccs      = {87, 93, 76, 90, 82, 92, 78, 7},
                .defaults = {3, 0, 64, 13, 0,  0,   0,  100},
            },
            .voiceMode = VoiceMode::Monophonic,
        },

        // 10. DX Classic E-Piano (Slot #11)
        {
            .model = SynthModel::DX_EPiano,
            .name = "DX E-PIANO",
            .description = "Yamaha DX7 Classic Electric Piano (Tine Strike & Warm Body)",
            .waveform = Waveform::Triangle,
            .subOscLevel = 0.0f, // Sub-osc off
            .detuneCents = 0.0f, // Detune cents
            .drive = 1.0f, // Clean
            .cutoffHz = 2800.0f, // Cutoff / Brightness
            .resonanceQ = 1.0f, // Ratio placeholder
            .filterEnvAmount = 0.5f, // Filter env amount
            .ampAttackMs = 5.0f, .ampDecayMs = 3000.0f, .ampSustain = 0.40f, .ampReleaseMs = 300.0f, // Amp ADSR: 5ms click-free attack, 3s piano decay, 0.4 sustain, 300ms release
            .filterAttackMs = 5.0f, .filterDecayMs = 400.0f, .filterSustain = 0.12f, .filterReleaseMs = 300.0f, // Modulator ADSR: 400ms tine decay, 0.12 sustain
            .chorusEnabled = true, .chorusRateHz = 0.85f, .chorusDepthMs = 2.5f, .chorusMix = 0.35f, // Stereo chorus
            .bankA = {
                .labels   = {"ModDepth", "Ratio", "Feedback", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {30, 5, 15, 44, 25, 118, 51, 75},
            },
            .bankB = {
                .labels   = {"ModDecay", "ModSust", "ModAtt", "Tremolo", "TremRate", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {78, 15, 27, 0,  34, 19, 56, 100},
            },
            .engineMode = EngineMode::FM,
            .modRatio = 1.0f, // modRatio (1:1 warm electric piano body)
            .modDepth = 2.80f, // modDepth (rich metallic chime)
            .modFeedback = 0.18f, // modFeedback (OP self-feedback warmth)
            .tremoloDepth = 0.0f,
            .tremoloRateHz = 4.5f,
        },

        // 11. Crystal Tines (Slot #12)
        {
            .model = SynthModel::FM_CrystalTines,
            .name = "FM TINES",
            .description = "Yamaha DX Celestial Bell Tines (Ratio 7:1 Chime Harpsichord)",
            .waveform = Waveform::Triangle,
            .subOscLevel = 0.0f,
            .detuneCents = 0.0f,
            .drive = 1.0f,
            .cutoffHz = 4500.0f,
            .resonanceQ = 7.0f,
            .filterEnvAmount = 0.5f,
            .ampAttackMs = 5.0f, .ampDecayMs = 3500.0f, .ampSustain = 0.50f, .ampReleaseMs = 450.0f,
            .filterAttackMs = 5.0f, .filterDecayMs = 750.0f, .filterSustain = 0.04f, .filterReleaseMs = 450.0f, // Modulator ADSR: sparkling bell decay
            .chorusEnabled = true, .chorusRateHz = 1.10f, .chorusDepthMs = 3.0f, .chorusMix = 0.45f, // Chorus
            .bankA = {
                .labels   = {"ModDepth", "Ratio", "Feedback", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {23, 61, 0,  57, 25, 120, 64, 83},
            },
            .bankB = {
                .labels   = {"ModDecay", "ModSust", "ModAtt", "Tremolo", "TremRate", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {92, 5, 27, 0,  38, 26, 70, 100},
            },
            .engineMode = EngineMode::FM,
            .modRatio = 7.0f, // modRatio (7:1 harmonic bell overtones)
            .modDepth = 2.20f,
            .modFeedback = 0.00f,
            .tremoloDepth = 0.0f,
            .tremoloRateHz = 5.0f,
        },

        // 12. Warm Rhodes (Slot #13)
        {
            .model = SynthModel::FM_WarmRhodes,
            .name = "RHODES",
            .description = "Vintage FM Suitcase Electric Piano with Stereo Pan Tremolo",
            .waveform = Waveform::Triangle,
            .subOscLevel = 0.0f,
            .detuneCents = 0.0f,
            .drive = 1.05f,
            .cutoffHz = 2400.0f,
            .resonanceQ = 1.0f,
            .filterEnvAmount = 0.5f,
            .ampAttackMs = 5.0f, .ampDecayMs = 2600.0f, .ampSustain = 0.55f, .ampReleaseMs = 250.0f,
            .filterAttackMs = 5.0f, .filterDecayMs = 300.0f, .filterSustain = 0.18f, .filterReleaseMs = 250.0f,
            .chorusEnabled = true, .chorusRateHz = 0.70f, .chorusDepthMs = 2.0f, .chorusMix = 0.25f,
            .bankA = {
                .labels   = {"ModDepth", "Ratio", "Feedback", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {16, 5, 30, 32, 25, 115, 70, 72},
            },
            .bankB = {
                .labels   = {"ModDecay", "ModSust", "ModAtt", "Tremolo", "TremRate", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {72, 23, 27, 48, 31, 16, 42, 100},
            },
            .engineMode = EngineMode::StageEp,
            .modRatio = 1.0f,
            .modDepth = 1.50f, // modDepth (mellow body)
            .modFeedback = 0.35f, // modFeedback (warmth)
            .tremoloDepth = 0.38f, // tremoloDepth (stereo suitcase pan tremolo)
            .tremoloRateHz = 4.20f,
        },

        // 13. FM Clavinet (Slot #14)
        {
            .model = SynthModel::FM_Clavinet,
            .name = "FM CLAVINET",
            .description = "Funky Snappy 2:1 FM Clavinet with Sharp Metallic Attack",
            .waveform = Waveform::Triangle,
            .subOscLevel = 0.0f,
            .detuneCents = 0.0f,
            .drive = 1.15f,
            .cutoffHz = 3800.0f,
            .resonanceQ = 2.0f,
            .filterEnvAmount = 0.5f,
            .ampAttackMs = 2.0f, .ampDecayMs = 850.0f, .ampSustain = 0.15f, .ampReleaseMs = 80.0f, // Snappy 2ms attack, fast release
            .filterAttackMs = 2.0f, .filterDecayMs = 160.0f, .filterSustain = 0.05f, .filterReleaseMs = 80.0f, // Sharp funk bite
            .chorusEnabled = false, .chorusRateHz = 0.90f, .chorusDepthMs = 1.5f, .chorusMix = 0.10f,
            .bankA = {
                .labels   = {"ModDepth", "Ratio", "Feedback", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {40, 14, 55, 10, 11,  94, 20, 51},
            },
            .bankB = {
                .labels   = {"ModDecay", "ModSust", "ModAtt", "Tremolo", "TremRate", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {59, 6, 12,  0,  47, 21, 28, 100},
            },
            .engineMode = EngineMode::FM,
            .modRatio = 2.0f, // modRatio (2:1 biting funk midrange)
            .modDepth = 3.80f, // modDepth (sharp funk attack)
            .modFeedback = 0.65f, // modFeedback (high feedback grit)
            .tremoloDepth = 0.0f,
            .tremoloRateHz = 6.0f,
        },

        // 14. FM Drawbar Organ (Slot #15)
        {
            .model = SynthModel::FM_DrawbarOrgan,
            .name = "FM ORGAN",
            .description = "Full Sustain Drawbar FM Organ with Rotary Speaker Leslie Chorus",
            .waveform = Waveform::Triangle,
            .subOscLevel = 0.0f,
            .detuneCents = 0.0f,
            .drive = 1.10f,
            .cutoffHz = 3200.0f,
            .resonanceQ = 1.0f,
            .filterEnvAmount = 0.5f,
            .ampAttackMs = 5.0f, .ampDecayMs = 50.0f, .ampSustain = 1.00f, .ampReleaseMs = 40.0f, // Full sustain organ
            .filterAttackMs = 5.0f, .filterDecayMs = 50.0f, .filterSustain = 1.00f, .filterReleaseMs = 40.0f,
            .chorusEnabled = true, .chorusRateHz = 1.80f, .chorusDepthMs = 3.5f, .chorusMix = 0.60f, // Rotary Leslie chorus
            .bankA = {
                .labels   = {"ModDepth", "Ratio", "Feedback", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {13, 5, 38, 76, 25, 42, 127, 38},
            },
            .bankB = {
                .labels   = {"ModDecay", "ModSust", "ModAtt", "Tremolo", "TremRate", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {34, 127, 27, 19, 48, 44, 85, 100},
            },
            .engineMode = EngineMode::FM,
            .modRatio = 1.0f,
            .modDepth = 1.20f,
            .modFeedback = 0.45f,
            .tremoloDepth = 0.15f, // tremoloDepth (simulated rotary vibrato)
            .tremoloRateHz = 6.20f,
        },

        // 15. PPG Wave 2.3 (Slot #16)
        [] {
            Preset p = makeEnginePreset(SynthModel::PPG_Wave, "PPG WAVE",
                "PPG Wave 2.3 Digital Wavetable Sweep into a 4-Pole Analog Filter", EngineMode::Wavetable,
                {{"Cutoff", "Resonnc", "WavePos", "WaveEnv", "Attack", "Decay", "Sustain", "Release"},
                 {74, 71, 40, 41, 73, 75, 79, 72},
                 {98, 27, 13, 89, 21, 104, 76, 88}},
                {{"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Table", "Detune", "Master"},
                 {76, 80, 81, 82, 83, 42, 94, 7},
                 {38, 11, 95, 25, 88, 0, 15, 100}});
            p.waveform = Waveform::Sawtooth; p.subOscLevel = 0.0f; p.detuneCents = 6.0f; p.drive = 1.0f;
            p.cutoffHz = 4200.0f; p.resonanceQ = 1.4f; p.filterEnvAmount = 0.30f;
            p.ampAttackMs = 4.0f; p.ampDecayMs = 1400.0f; p.ampSustain = 0.60f; p.ampReleaseMs = 600.0f;
            p.filterAttackMs = 2.0f; p.filterDecayMs = 900.0f; p.filterSustain = 0.20f; p.filterReleaseMs = 600.0f;
            p.chorusEnabled = true; p.chorusRateHz = 0.6f; p.chorusDepthMs = 2.5f; p.chorusMix = 0.35f;
            p.timbre.wtTable = 0; p.timbre.wtPosition = 0.10f; p.timbre.wtEnvAmount = 0.70f;
            return p;
        }(),

        // 16. Waldorf Microwave style vocal wavetable pad (Slot #17)
        [] {
            Preset p = makeEnginePreset(SynthModel::WT_VoxPad, "WAVE VOX",
                "Waldorf Microwave Style Formant Wavetable Pad (Slow Vowel Morph)", EngineMode::Wavetable,
                {{"Cutoff", "Resonnc", "WavePos", "WaveEnv", "Attack", "Decay", "Sustain", "Release"},
                 {74, 71, 40, 41, 73, 75, 79, 72},
                 {89, 33, 19, 76, 98, 110, 108, 112}},
                {{"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Table", "Detune", "Master"},
                 {76, 80, 81, 82, 83, 42, 94, 7},
                 {32, 115, 118, 64, 114, 80, 30, 100}});
            p.waveform = Waveform::Sawtooth; p.subOscLevel = 0.0f; p.detuneCents = 12.0f; p.drive = 1.0f;
            p.cutoffHz = 2600.0f; p.resonanceQ = 1.8f; p.filterEnvAmount = 0.25f;
            p.ampAttackMs = 600.0f; p.ampDecayMs = 2000.0f; p.ampSustain = 0.85f; p.ampReleaseMs = 2200.0f;
            p.filterAttackMs = 1800.0f; p.filterDecayMs = 3000.0f; p.filterSustain = 0.50f; p.filterReleaseMs = 2500.0f;
            p.chorusEnabled = true; p.chorusRateHz = 0.4f; p.chorusDepthMs = 3.5f; p.chorusMix = 0.55f;
            p.timbre.wtTable = 2; p.timbre.wtPosition = 0.15f; p.timbre.wtEnvAmount = 0.60f;
            return p;
        }(),

        // 17. Casio CZ-101 bass (Slot #18)
        [] {
            Preset p = makeEnginePreset(SynthModel::CZ_Bass, "CZ BASS",
                "Casio CZ-101 Phase Distortion Saw Bass (Snappy DCW Envelope)", EngineMode::PhaseDist,
                {{"DCW", "DCW-Env", "Wave", "Detune", "Attack", "Decay", "Sustain", "Release"},
                 {43, 76, 44, 94, 73, 75, 79, 72},
                 {13, 102, 0, 0, 6, 95, 70, 58}},
                {{"DCW-Att", "DCW-Dec", "DCW-Sus", "DCW-Rel", "Chorus", "VibRate", "VibDepth", "Master"},
                 {80, 81, 82, 83, 78, 89, 90, 7},
                 {0, 74, 13, 58, 0, 44, 0, 100}});
            p.detuneCents = 0.0f; p.filterEnvAmount = 0.80f;
            p.ampAttackMs = 1.5f; p.ampDecayMs = 900.0f; p.ampSustain = 0.55f; p.ampReleaseMs = 120.0f;
            p.filterAttackMs = 1.0f; p.filterDecayMs = 280.0f; p.filterSustain = 0.10f; p.filterReleaseMs = 120.0f;
            p.chorusEnabled = false; p.chorusRateHz = 0.8f; p.chorusDepthMs = 2.0f; p.chorusMix = 0.0f;
            p.timbre.pdWave = PdWave::Saw; p.timbre.pdDcw = 0.10f;
            return p;
        }(),

        // 18. Casio CZ-101 resonance lead (Slot #19)
        [] {
            Preset p = makeEnginePreset(SynthModel::CZ_Reso, "CZ RESO",
                "Casio CZ-101 Resonance Wave Lead (Two Detuned Lines, DCW Sweep)", EngineMode::PhaseDist,
                {{"DCW", "DCW-Env", "Wave", "Detune", "Attack", "Decay", "Sustain", "Release"},
                 {43, 76, 44, 94, 73, 75, 79, 72},
                 {19, 70, 82, 23, 38, 105, 95, 83}},
                {{"DCW-Att", "DCW-Dec", "DCW-Sus", "DCW-Rel", "Chorus", "VibRate", "VibDepth", "Master"},
                 {80, 81, 82, 83, 78, 89, 90, 7},
                 {63, 95, 44, 85, 44, 44, 0, 100}});
            p.detuneCents = 9.0f; p.filterEnvAmount = 0.55f;
            p.ampAttackMs = 12.0f; p.ampDecayMs = 1500.0f; p.ampSustain = 0.75f; p.ampReleaseMs = 450.0f;
            p.filterAttackMs = 60.0f; p.filterDecayMs = 900.0f; p.filterSustain = 0.35f; p.filterReleaseMs = 500.0f;
            p.chorusEnabled = true; p.chorusRateHz = 0.7f; p.chorusDepthMs = 2.5f; p.chorusMix = 0.35f;
            p.timbre.pdWave = PdWave::ResoSaw; p.timbre.pdDcw = 0.15f;
            return p;
        }(),

        // 19. Karplus-Strong steel-string guitar (Slot #20)
        [] {
            Preset p = makeEnginePreset(SynthModel::KS_Guitar, "KS GUITAR",
                "Karplus-Strong Plucked Steel-String Guitar (Physical Model)", EngineMode::Pluck,
                {{"StrDecay", "Bright", "PickPos", "Chorus", "Attack", "Drive", "VibDepth", "Release"},
                 {45, 46, 47, 78, 73, 87, 90, 72},
                 {98, 70, 28, 25, 0, 0, 0, 66}},
                {{"ChoRate", "ChoDepth", "VibRate", "Glide", "Tremolo", "TremRate", "PolyMode", "Master"},
                 {84, 85, 89, 93, 53, 54, 92, 7},
                 {10, 42, 44, 0, 0, 34, 127, 100}});
            p.drive = 1.0f;
            p.ampAttackMs = 1.0f; p.ampDecayMs = 300.0f; p.ampSustain = 1.0f; p.ampReleaseMs = 180.0f;
            p.chorusEnabled = true; p.chorusRateHz = 0.5f; p.chorusDepthMs = 2.0f; p.chorusMix = 0.20f;
            p.tremoloRateHz = 4.5f;
            p.timbre.pluckDecayMs = 3500.0f; p.timbre.pluckBright = 0.55f; p.timbre.pluckPick = 0.22f;
            return p;
        }(),

        // 20. Karplus-Strong koto (Slot #21)
        [] {
            Preset p = makeEnginePreset(SynthModel::KS_Koto, "KS KOTO",
                "Karplus-Strong Bright Koto / Harp Pluck (Near-Bridge Pick)", EngineMode::Pluck,
                {{"StrDecay", "Bright", "PickPos", "Chorus", "Attack", "Drive", "VibDepth", "Release"},
                 {45, 46, 47, 78, 73, 87, 90, 72},
                 {85, 108, 10, 0, 0, 0, 0, 81}},
                {{"ChoRate", "ChoDepth", "VibRate", "Glide", "Tremolo", "TremRate", "PolyMode", "Master"},
                 {84, 85, 89, 93, 53, 54, 92, 7},
                 {10, 42, 44, 0, 0, 34, 127, 100}});
            p.drive = 1.0f;
            p.ampAttackMs = 1.0f; p.ampDecayMs = 300.0f; p.ampSustain = 1.0f; p.ampReleaseMs = 400.0f;
            p.chorusEnabled = false; p.chorusRateHz = 0.5f; p.chorusDepthMs = 2.0f; p.chorusMix = 0.0f;
            p.tremoloRateHz = 4.5f;
            p.timbre.pluckDecayMs = 2200.0f; p.timbre.pluckBright = 0.85f; p.timbre.pluckPick = 0.08f;
            return p;
        }(),

        // 21. Hammond B-3 jazz (Slot #22)
        [] {
            Preset p = makeEnginePreset(SynthModel::B3_Jazz, "B3 JAZZ",
                "Hammond B-3 Additive Drawbars 888000000, 3rd Percussion, Slow Leslie", EngineMode::Additive,
                {{"16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'"},
                 {55, 56, 57, 58, 59, 60, 61, 62},
                 {127, 127, 127, 0, 0, 0, 0, 0}},
                {{"1'", "Perc", "PercHarm", "PercDec", "Click", "Leslie", "LeslRate", "Master"},
                 {63, 48, 49, 81, 51, 53, 54, 7},
                 {0, 76, 127, 81, 44, 23, 3, 100}});
            p.ampAttackMs = 3.0f; p.ampDecayMs = 50.0f; p.ampSustain = 1.0f; p.ampReleaseMs = 40.0f;
            p.filterAttackMs = 1.0f; p.filterDecayMs = 400.0f; p.filterSustain = 0.0f; p.filterReleaseMs = 40.0f;
            p.chorusEnabled = true; p.chorusRateHz = 2.5f; p.chorusDepthMs = 1.5f; p.chorusMix = 0.30f;
            p.tremoloDepth = 0.18f; p.tremoloRateHz = 0.8f;
            const float bars[DRAWBAR_COUNT] = {8, 8, 8, 0, 0, 0, 0, 0, 0};
            std::copy(bars, bars + DRAWBAR_COUNT, p.timbre.drawbars);
            p.timbre.percLevel = 0.60f; p.timbre.percHarmonic = 1; p.timbre.keyClick = 0.35f;
            return p;
        }(),

        // 22. Hammond B-3 gospel (Slot #23)
        [] {
            Preset p = makeEnginePreset(SynthModel::B3_Gospel, "B3 GOSPEL",
                "Hammond B-3 Additive Drawbars 888808008, Key Click, Fast Leslie", EngineMode::Additive,
                {{"16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'"},
                 {55, 56, 57, 58, 59, 60, 61, 62},
                 {127, 127, 127, 127, 0, 127, 0, 0}},
                {{"1'", "Perc", "PercHarm", "PercDec", "Click", "Leslie", "LeslRate", "Master"},
                 {63, 48, 49, 81, 51, 53, 54, 7},
                 {127, 0, 0, 95, 64, 44, 51, 100}});
            p.ampAttackMs = 3.0f; p.ampDecayMs = 50.0f; p.ampSustain = 1.0f; p.ampReleaseMs = 40.0f;
            p.filterAttackMs = 1.0f; p.filterDecayMs = 900.0f; p.filterSustain = 0.0f; p.filterReleaseMs = 40.0f;
            p.chorusEnabled = true; p.chorusRateHz = 2.5f; p.chorusDepthMs = 1.5f; p.chorusMix = 0.25f;
            p.tremoloDepth = 0.35f; p.tremoloRateHz = 6.5f;
            const float bars[DRAWBAR_COUNT] = {8, 8, 8, 8, 0, 8, 0, 0, 8};
            std::copy(bars, bars + DRAWBAR_COUNT, p.timbre.drawbars);
            p.timbre.percLevel = 0.0f; p.timbre.percHarmonic = 0; p.timbre.keyClick = 0.50f;
            return p;
        }(),

        // 23. ARP Odyssey duophonic lead (Slot #24)
        {
            .model = SynthModel::Odyssey,
            .name = "ODYSSEY",
            .description = "ARP Odyssey Hard Sync Lead & Ring Modulator",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.35f,
            .detuneCents = 12.0f,
            .drive = 1.20f,
            .cutoffHz = 3200.0f,
            .resonanceQ = 3.0f,
            .filterEnvAmount = 0.60f,
            .ampAttackMs = 5.0f, .ampDecayMs = 300.0f, .ampSustain = 0.80f, .ampReleaseMs = 200.0f,
            .filterAttackMs = 5.0f, .filterDecayMs = 450.0f, .filterSustain = 0.20f, .filterReleaseMs = 200.0f,
            .chorusEnabled = false, .chorusRateHz = 0.5f, .chorusDepthMs = 2.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {93, 45, 44, 0,  25, 75, 102, 68},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Drive", "Glide", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 87, 93, 7},
                .defaults = {76, 25, 83, 25, 68, 10, 0,  100},
            },
            .voiceMode = VoiceMode::Monophonic,
        },

        // 24. Roland Jupiter-8 cross-modulated poly (Slot #25)
        {
            .model = SynthModel::VintageJup,
            .name = "VINTAGE JUP",
            .description = "Roland Jupiter-8 Cross-Modulated Dual Saw Poly Synth",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.25f,
            .detuneCents = 8.0f,
            .drive = 1.10f,
            .cutoffHz = 2400.0f,
            .resonanceQ = 2.2f,
            .filterEnvAmount = 0.45f,
            .ampAttackMs = 15.0f, .ampDecayMs = 400.0f, .ampSustain = 0.75f, .ampReleaseMs = 350.0f,
            .filterAttackMs = 20.0f, .filterDecayMs = 500.0f, .filterSustain = 0.35f, .filterReleaseMs = 350.0f,
            .chorusEnabled = true, .chorusRateHz = 0.5f, .chorusDepthMs = 2.5f, .chorusMix = 0.35f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {88, 37, 32, 44, 41, 81, 95, 78},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Drive", "ChoRate", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 87, 84, 7},
                .defaults = {57, 46, 85, 44, 78, 5,  10, 100},
            },
        },

        // 25. Tube-saturated poly synth (Slot #26)
        {
            .model = SynthModel::TubeSynth,
            .name = "TUBESYNTH",
            .description = "Triode Tube Saturated Asymmetric Poly Lead & Pad",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.20f,
            .detuneCents = 10.0f,
            .drive = 2.20f,
            .cutoffHz = 1800.0f,
            .resonanceQ = 2.5f,
            .filterEnvAmount = 0.50f,
            .ampAttackMs = 25.0f, .ampDecayMs = 350.0f, .ampSustain = 0.80f, .ampReleaseMs = 300.0f,
            .filterAttackMs = 30.0f, .filterDecayMs = 450.0f, .filterSustain = 0.30f, .filterReleaseMs = 300.0f,
            .chorusEnabled = false, .chorusRateHz = 0.5f, .chorusDepthMs = 2.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "Drive", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 87, 78, 73, 75, 79, 72},
                .defaults = {83, 41, 61, 0,  49, 78, 102, 75},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Glide", "SubOsc", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 93, 77, 7},
                .defaults = {64, 52, 83, 38, 75, 0,  25,  100},
            },
        },

        // 26. TR-808 sub bass (Slot #27)
        {
            .model = SynthModel::SubFactory,
            .name = "SUB FACTORY",
            .description = "TR-808 Sub-Bass with Two-Stage Pitch Envelope & Tube Drive",
            .waveform = Waveform::Triangle,
            .subOscLevel = 0.0f,
            .detuneCents = 0.0f,
            .drive = 1.80f,
            .cutoffHz = 650.0f,
            .resonanceQ = 1.2f,
            .filterEnvAmount = 0.70f,
            .ampAttackMs = 2.0f, .ampDecayMs = 800.0f, .ampSustain = 0.0f, .ampReleaseMs = 300.0f,
            .filterAttackMs = 5.0f, .filterDecayMs = 120.0f, .filterSustain = 0.0f, .filterReleaseMs = 100.0f,
            .chorusEnabled = false, .chorusRateHz = 0.5f, .chorusDepthMs = 2.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Drive", "Resonnc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 87, 71, 78, 73, 75, 79, 72},
                .defaults = {64, 41, 24, 0,  11, 93,  0,  75},
            },
            .bankB = {
                .labels   = {"FiltEnv", "SubOsc", "Glide", "ChoRate", "ChoDepth", "F-Attack", "F-Decay", "Master"},
                .ccs      = {76, 77, 93, 84, 85, 80, 81, 7},
                .defaults = {89, 0,  0,  10, 42, 25, 58,  100},
            },
            .voiceMode = VoiceMode::Monophonic,
        },

        // 27. Memorymoog 3-oscillator brass (Slot #28)
        {
            .model = SynthModel::Memorymoog,
            .name = "MEMORYMOOG",
            .description = "Massive 3-Oscillator Detuned Ladder Filter Poly Brass",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.50f,
            .detuneCents = 16.0f,
            .drive = 1.35f,
            .cutoffHz = 1600.0f,
            .resonanceQ = 2.0f,
            .filterEnvAmount = 0.65f,
            .ampAttackMs = 20.0f, .ampDecayMs = 450.0f, .ampSustain = 0.70f, .ampReleaseMs = 400.0f,
            .filterAttackMs = 35.0f, .filterDecayMs = 600.0f, .filterSustain = 0.35f, .filterReleaseMs = 400.0f,
            .chorusEnabled = false, .chorusRateHz = 0.5f, .chorusDepthMs = 2.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {81, 35, 64, 0,  46, 83, 89, 81},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Drive", "Glide", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 87, 93, 7},
                .defaults = {83, 54, 88, 44, 81, 18, 0,  100},
            },
        },

        // 28. SH-101 acid/electro bass (Slot #29)
        {
            .model = SynthModel::Iona,
            .name = "IONA",
            .description = "SH-101 Snappy Electro & Acid Monosynth with Sub-Oscillator",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.65f,
            .detuneCents = 0.0f,
            .drive = 1.15f,
            .cutoffHz = 2200.0f,
            .resonanceQ = 3.5f,
            .filterEnvAmount = 0.75f,
            .ampAttackMs = 2.0f, .ampDecayMs = 250.0f, .ampSustain = 0.50f, .ampReleaseMs = 150.0f,
            .filterAttackMs = 3.0f, .filterDecayMs = 300.0f, .filterSustain = 0.10f, .filterReleaseMs = 150.0f,
            .chorusEnabled = false, .chorusRateHz = 0.5f, .chorusDepthMs = 2.0f, .chorusMix = 0.0f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {86, 50, 83, 0,  11, 72, 64, 63},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Glide", "Drive", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 93, 87, 7},
                .defaults = {95, 17, 75, 13, 63, 0,  8,   100},
            },
            .voiceMode = VoiceMode::Monophonic,
        },

        // 29. OPx-4 FM synthesizer (Slot #30)
        [] {
            Preset p = makeDefaultUserPreset(SynthModel::OPx4, "OPX-4 FM",
                "4-Operator FM Synthesizer with 8 Algorithmic Routing Matrix");
            p.engineMode = EngineMode::Opx4;
            p.modRatio = 2.0f;
            p.modDepth = 3.0f;
            p.modFeedback = 0.40f;
            p.timbre.opxAlgo = 0;
            p.ampAttackMs = 5.0f; p.ampDecayMs = 600.0f; p.ampSustain = 0.60f; p.ampReleaseMs = 300.0f;
            p.filterAttackMs = 5.0f; p.filterDecayMs = 400.0f; p.filterSustain = 0.20f; p.filterReleaseMs = 300.0f;
            p.bankA = {
                .labels   = {"ModDepth", "Ratio", "Feedback", "Algo", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 50, 73, 75, 79, 72},
                .defaults = {32, 14, 34, 0,  25, 88, 76, 75},
            };
            p.bankB = {
                .labels   = {"ModDecay", "ModSust", "ModAtt", "Chorus", "ChoRate", "ChoDepth", "PolyMode", "Master"},
                .ccs      = {76, 80, 81, 78, 84, 85, 92, 7},
                .defaults = {78, 25, 27, 10, 10, 56, 127, 100},
            };
            return p;
        }(),

        // 30. ARP Solina String Ensemble (Slot #31)
        {
            .model = SynthModel::Solina,
            .name = "SOLINA STR",
            .description = "ARP Solina String Ensemble with Violin/Horn Formants & 3-Phase Chorus",
            .waveform = Waveform::Sawtooth,
            .subOscLevel = 0.0f,
            .detuneCents = 4.0f,
            .drive = 1.0f,
            .cutoffHz = 6000.0f,
            .resonanceQ = 1.0f,
            .filterEnvAmount = 0.0f,
            .ampAttackMs = 60.0f, .ampDecayMs = 400.0f, .ampSustain = 0.90f, .ampReleaseMs = 450.0f,
            .filterAttackMs = 80.0f, .filterDecayMs = 400.0f, .filterSustain = 0.90f, .filterReleaseMs = 450.0f,
            .chorusEnabled = true, .chorusRateHz = 0.6f, .chorusDepthMs = 2.5f, .chorusMix = 0.85f,
            .bankA = {
                .labels   = {"Cutoff", "Resonnc", "Chorus", "SubOsc", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 78, 77, 73, 75, 79, 72},
                .defaults = {105, 20, 108, 0,  63, 81, 114, 83},
            },
            .bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {0,  67, 81, 114, 83, 13, 56, 100},
            },
        },

        // 31. Commuted Waveguide Grand Piano (Slot #32)
        [] {
            Preset p = makeDefaultUserPreset(SynthModel::StagePiano, "STAGE PIANO",
                "Commuted Waveguide Acoustic Grand Piano with Dispersion");
            p.engineMode = EngineMode::StagePiano;
            p.drive = 1.0f;
            p.cutoffHz = 5000.0f;
            p.ampAttackMs = 1.0f; p.ampDecayMs = 3000.0f; p.ampSustain = 0.30f; p.ampReleaseMs = 250.0f;
            p.timbre.pluckDecayMs = 4000.0f; p.timbre.pluckBright = 0.70f;
            p.bankA = {
                .labels   = {"StrDecay", "Bright", "Chorus", "Drive", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {45, 74, 78, 87, 73, 75, 79, 72},
                .defaults = {102, 102, 10, 0,  0,  118, 38, 72},
            };
            p.bankB = {
                .labels   = {"ChoRate", "ChoDepth", "VibRate", "Glide", "Tremolo", "TremRate", "PolyMode", "Master"},
                .ccs      = {84, 85, 89, 93, 53, 54, 92, 7},
                .defaults = {10, 56, 44, 0,  0,  34, 127, 100},
            };
            return p;
        }(),

        // 32. Studio Strings Section (Slot #33)
        [] {
            Preset p = makeDefaultUserPreset(SynthModel::StudioStrings, "STUDIO STR",
                "Studio Bowed String Section with Rosin Friction Noise & Formants");
            p.engineMode = EngineMode::StudioStrings;
            p.detuneCents = 8.0f;
            p.drive = 1.0f;
            p.cutoffHz = 4500.0f;
            p.ampAttackMs = 80.0f; p.ampDecayMs = 400.0f; p.ampSustain = 0.85f; p.ampReleaseMs = 350.0f;
            p.chorusEnabled = true; p.chorusMix = 0.40f;
            p.bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {100, 35, 32, 51, 67, 81, 108, 78},
            };
            p.bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {64, 46, 78, 38, 75, 10, 56, 100},
            };
            return p;
        }(),

        // 33. Fabric XL Pad (Slot #34)
        [] {
            Preset p = makeDefaultUserPreset(SynthModel::FabricXl, "FABRIC XL",
                "Dual Layer Wavetable Pad with Harmonic Chime & Breath Transients");
            p.engineMode = EngineMode::FabricXl;
            p.detuneCents = 6.0f;
            p.drive = 1.0f;
            p.cutoffHz = 3500.0f;
            p.ampAttackMs = 90.0f; p.ampDecayMs = 500.0f; p.ampSustain = 0.85f; p.ampReleaseMs = 450.0f;
            p.chorusEnabled = true; p.chorusMix = 0.55f;
            p.bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {95, 35, 32, 70, 69, 85, 108, 83},
            };
            p.bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ChoRate", "ChoDepth", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 84, 85, 7},
                .defaults = {64, 46, 78, 38, 75, 10, 56, 100},
            };
            return p;
        }(),

        // 34. Mariana Bass (Slot #35)
        [] {
            Preset p = makeDefaultUserPreset(SynthModel::Mariana, "MARIANA",
                "Dual-Path Coherent Bass with Phase-Locked Sine Sub & Saturated Mid");
            p.engineMode = EngineMode::Mariana;
            p.subOscLevel = 0.80f;
            p.drive = 1.80f;
            p.cutoffHz = 1200.0f;
            p.resonanceQ = 1.8f;
            p.ampAttackMs = 5.0f; p.ampDecayMs = 350.0f; p.ampSustain = 0.70f; p.ampReleaseMs = 180.0f;
            p.voiceMode = VoiceMode::Monophonic;
            p.bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Drive", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 87, 73, 75, 79, 72},
                .defaults = {75, 33, 102, 41, 25, 78, 89, 66},
            };
            p.bankB = {
                .labels   = {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "Glide", "PolyMode", "Master"},
                .ccs      = {76, 80, 81, 82, 83, 93, 92, 7},
                .defaults = {64, 46, 78, 38, 75, 0,  0,  100},
            };
            return p;
        }(),

        // 35. Hype Saw (Slot #36)
        [] {
            Preset p = makeDefaultUserPreset(SynthModel::Hype, "HYPE SAW",
                "Phase-Scrambled Detuned Hypersaw with Comb Stereoizer & Pump");
            p.engineMode = EngineMode::Hype;
            p.detuneCents = 14.0f;
            p.tremoloDepth = 0.75f;
            p.tremoloRateHz = 2.0f;
            p.cutoffHz = 5500.0f;
            p.resonanceQ = 1.5f;
            p.ampAttackMs = 5.0f; p.ampDecayMs = 400.0f; p.ampSustain = 0.85f; p.ampReleaseMs = 250.0f;
            p.bankA = {
                .labels   = {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
                .ccs      = {74, 71, 77, 78, 73, 75, 79, 72},
                .defaults = {103, 29, 32, 10, 25, 81, 108, 72},
            };
            p.bankB = {
                .labels   = {"Tremolo", "TremRate", "Drive", "Glide", "VibRate", "VibDepth", "PolyMode", "Master"},
                .ccs      = {53, 54, 87, 93, 89, 90, 92, 7},
                .defaults = {95, 13, 0,  0,  44, 0,  127, 100},
            };
            return p;
        }(),

        // 36..44. User Customizable Slots (Slots #37..#45)
        makeDefaultUserPreset(SynthModel::User_01, "USER 01", "Custom User Defined Patch (Slot #37 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_02, "USER 02", "Custom User Defined Patch (Slot #38 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_03, "USER 03", "Custom User Defined Patch (Slot #39 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_04, "USER 04", "Custom User Defined Patch (Slot #40 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_05, "USER 05", "Custom User Defined Patch (Slot #41 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_06, "USER 06", "Custom User Defined Patch (Slot #42 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_07, "USER 07", "Custom User Defined Patch (Slot #43 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_08, "USER 08", "Custom User Defined Patch (Slot #44 NVS Storage)"),
        makeDefaultUserPreset(SynthModel::User_09, "USER 09", "Custom User Defined Patch (Slot #45 NVS Storage)")
    }};
    return presets;
}

} // namespace Synth
