#pragma once
#include <cstring>
#include <cstdint>

#include <cstddef>
#include <cmath>
#include <algorithm>
#include "MidiMapping.h"

namespace Synth {

enum class TimeSignaturePreset : uint8_t {
    TS_4_4  = 0, // 4/4 Common time
    TS_3_4  = 1, // 3/4 Waltz
    TS_2_4  = 2, // 2/4 March / Polka
    TS_5_4  = 3, // 5/4 Odd meter
    TS_6_8  = 4, // 6/8 Compound duple (accents on 1, 4)
    TS_7_8  = 5, // 7/8 Odd compound (accents on 1, 4, 6)
    TS_9_8  = 6, // 9/8 Compound triple (accents on 1, 4, 7)
    TS_12_8 = 7, // 12/8 Compound quadruple (accents on 1, 4, 7, 10)
    Count   = 8
};

enum class ScaleMode : uint8_t {
    Chromatic       = 0, // All 12 semitones (no quantization)
    Major           = 1, // Ionian: 0, 2, 4, 5, 7, 9, 11
    NaturalMinor    = 2, // Aeolian: 0, 2, 3, 5, 7, 8, 10
    HarmonicMinor   = 3, // 0, 2, 3, 5, 7, 8, 11
    Dorian          = 4, // 0, 2, 3, 5, 7, 9, 10
    Mixolydian      = 5, // 0, 2, 4, 5, 7, 9, 10
    MajorPentatonic = 6, // 0, 2, 4, 7, 9
    MinorPentatonic = 7, // 0, 3, 5, 7, 10
    Blues           = 8, // 0, 3, 5, 6, 7, 10
    Count           = 9
};

enum class SubModMode : uint8_t {
    None         = 0,
    HardSyncRing = 1, // Odyssey: process_hard_sync_saw + ring_modulate
    CrossMod     = 2, // VintageJup: cross_modulate
    TubeDrive    = 3, // TubeSynth: tube_saturate_sample
    Sub808       = 4, // SubFactory: PitchEnvelope808 + tube drive
};

enum class EngineMode : uint8_t {
    Subtractive   = 0,
    FM            = 1,
    Wavetable     = 2, // PPG / Waldorf: morphing single-cycle tables into the filter
    PhaseDist     = 3, // Casio CZ: cosine read through a warped phase, DCW envelope
    Pluck         = 4, // Karplus-Strong string: noise burst into a tuned, damped delay loop
    Additive      = 5, // Hammond drawbars: nine sine partials, percussion and key click
    Opx4          = 6, // 4-operator FM (8 algorithms)
    StagePiano    = 7, // Commuted waveguide grand piano
    StageEp       = 8, // Modal bark electric piano + 4-stage phaser
    StudioStrings = 9, // Bowed strings with rosin noise + 3 body formants
    FabricXl      = 10, // Dual wavetable + chime & breath transients
    Mariana       = 11, // Dual-path phase-locked sub + tube mid bass
    Hype          = 12, // Phase-scrambled hypersaw + comb stereoizer + pump
    Count         = 13
};

// Casio CZ DCO waveforms (phase distortion engine)
enum class PdWave : uint8_t {
    Saw       = 0,
    Square    = 1,
    Pulse     = 2,
    DoubleSine = 3,
    ResoSaw   = 4, // Resonance I: sine burst under a falling-saw window
    ResoTri   = 5, // Resonance II: triangle window
    ResoTrap  = 6, // Resonance III: trapezoid window
    Count     = 7
};

constexpr size_t WAVETABLE_COUNT = 4;  // Wavetable banks (see Wavetables.h)
constexpr size_t DRAWBAR_COUNT   = 9;  // 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'

// Patch parameters of the wavetable, phase distortion, pluck and additive engines. The engines also
// use the shared fields of a patch: amp envelope, filter envelope (wave position / DCW / percussion
// envelope), filter, detune, drive, chorus and tremolo.
struct TimbreParams {
    // Wavetable
    uint8_t wtTable{0};           // 0..WAVETABLE_COUNT-1
    float   wtPosition{0.0f};     // 0..1 across the table's frames
    float   wtEnvAmount{0.5f};    // filter envelope -> position, 0..1
    // Phase distortion
    PdWave  pdWave{PdWave::Saw};
    float   pdDcw{0.3f};          // base distortion 0..1 (the filter envelope adds filterEnvAmount)
    // Pluck
    float   pluckDecayMs{3000.0f}; // T60 of the string's fundamental
    float   pluckBright{0.6f};     // excitation and loop brightness 0..1
    float   pluckPick{0.25f};      // pick position along the string 0..1 (0.5 = middle)
    // Additive
    float   drawbars[DRAWBAR_COUNT]{8, 8, 8, 0, 0, 0, 0, 0, 0}; // 0..8 per drawbar
    float   percLevel{0.0f};       // percussion level 0..1 (filter envelope is its decay)
    uint8_t percHarmonic{1};       // 0 = second (4'), 1 = third (2 2/3')
    float   keyClick{0.3f};        // key click level 0..1
    // OPx4
    uint8_t opxAlgo{0};            // 0..7
};

enum class VoiceMode : uint8_t {
    Polyphonic = 0,
    Monophonic = 1
};

enum class NotePriority : uint8_t {
    Last = 0,
    Low  = 1,
    High = 2
};

enum class Waveform : uint8_t {
    Sawtooth = 0,
    Square   = 1,
    Triangle = 2,
    Count    = 3
};

struct SynthConfig {
    static constexpr float DEFAULT_SAMPLE_RATE = 44100.0f;
    static constexpr size_t DEFAULT_VOICE_COUNT = 8;
    static constexpr size_t BLOCK_SIZE = 128;
};

// Parameter scaling helpers
class ParameterScaler {
public:
    // Convert 7-bit MIDI CC [0..127] to normalized float [0.0..1.0]
    static inline float ccToNorm(uint8_t cc) {
        return static_cast<float>(cc) / 127.0f;
    }

    // Linear-to-exponential cutoff frequency mapping:
    // cc = 0 -> minHz (20 Hz), cc = 127 -> maxHz (20000 Hz)
    static inline float ccToCutoffHz(uint8_t cc, float minHz = 20.0f, float maxHz = 20000.0f) {
        float norm = ccToNorm(cc);
        return minHz * std::pow(maxHz / minHz, norm);
    }

    // Linear-to-exponential envelope time mapping:
    // cc = 0 -> minMs (1 ms), cc = 127 -> maxMs (5000 ms)
    static inline float ccToEnvelopeTimeMs(uint8_t cc, float minMs = 1.0f, float maxMs = 5000.0f) {
        float norm = ccToNorm(cc);
        return minMs * std::pow(maxMs / minMs, norm);
    }

    // Linear-to-exponential resonance (Q) mapping:
    // cc = 0 -> 0.5 (gentle), cc = 127 -> 20.0 (high self-oscillation)
    static inline float ccToResonanceQ(uint8_t cc, float minQ = 0.5f, float maxQ = 20.0f) {
        float norm = ccToNorm(cc);
        // Quadratic shaping gives smooth response in the musically useful 0.7-5 range
        return minQ + (maxQ - minQ) * (norm * norm);
    }

    // MIDI Note number [0..127] to frequency in Hz (A4 = 440 Hz = MIDI note 69)
    static inline float midiNoteToHz(uint8_t note, float detuneSemitones = 0.0f) {
        float n = static_cast<float>(note) + detuneSemitones;
        return 440.0f * std::pow(2.0f, (n - 69.0f) / 12.0f);
    }

    // Inverse mappings: convert parameter values back to 7-bit MIDI CC [0..127]
    static inline uint8_t normToCC(float norm) {
        if (norm <= 0.0f) return 0;
        if (norm >= 1.0f) return 127;
        return static_cast<uint8_t>(std::round(norm * 127.0f));
    }

    static inline uint8_t cutoffHzToCC(float hz, float minHz = 20.0f, float maxHz = 20000.0f) {
        float clamped = std::clamp(hz, minHz, maxHz);
        float norm = (std::log10(clamped) - std::log10(minHz)) / (std::log10(maxHz) - std::log10(minHz));
        return normToCC(norm);
    }

    static inline uint8_t resonanceQToCC(float q, float minQ = 0.5f, float maxQ = 20.0f) {
        float clamped = std::clamp(q, minQ, maxQ);
        float norm = std::sqrt((clamped - minQ) / (maxQ - minQ));
        return normToCC(norm);
    }

    static inline uint8_t envelopeTimeMsToCC(float ms, float minMs = 1.0f, float maxMs = 5000.0f) {
        float clamped = std::clamp(ms, minMs, maxMs);
        float norm = (std::log10(clamped) - std::log10(minMs)) / (std::log10(maxMs) - std::log10(minMs));
        return normToCC(norm);
    }

    // Smooth, continuously differentiable soft-clipping saturation asymptotic to [-1.0, 1.0]
    static inline float softClip(float x) {
        return x / std::sqrt(1.0f + x * x);
    }

    // 2^x without libm (about 2.8 us per std::exp2 call on the ESP32-S3, which dominated the per-voice
    // control-rate update). Range-reduces to [-0.5, 0.5] and evaluates a degree-5 Taylor series of
    // e^(f ln2): relative error < 3e-6 (0.005 cent), far below anything audible for pitch or cutoff.
    static inline float fastExp2(float x) {
        x = std::clamp(x, -120.0f, 120.0f);
        const int   i = static_cast<int>(x + (x >= 0.0f ? 0.5f : -0.5f));
        const float f = x - static_cast<float>(i);
        const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f +
                        f * (0.00961813f + f * 0.00133336f))));
        const uint32_t bits = static_cast<uint32_t>(i + 127) << 23;
        float scale;
        std::memcpy(&scale, &bits, sizeof(scale));
        return p * scale;
    }

    // 1/d for normal, finite d (no zero/inf/NaN handling). GCC lowers a float division on the
    // ESP32-S3 to a call into the ROM __divsf3 (about 250 ns); here the FPU's RECIP0.S seed plus two
    // fused Newton steps gives the same value to within an ulp or two in a few cycles. Elsewhere it is
    // a plain division, so desktop output is unchanged.
    static inline float fastRecip(float d) {
#if defined(__XTENSA__) && defined(__XCHAL_HAVE_FP_RECIP) && __XCHAL_HAVE_FP_RECIP
        float r;
        __asm__("recip0.s %0, %1" : "=f"(r) : "f"(d));
        r = __builtin_fmaf(r, __builtin_fmaf(-d, r, 1.0f), r);
        r = __builtin_fmaf(r, __builtin_fmaf(-d, r, 1.0f), r);
        return r;
#else
        return 1.0f / d;
#endif
    }

    // Fast tan for x in (0, ~0.45*pi] (filter prewarp). Pade [5/4] of tan below pi/4, and
    // tan(x) = 1/tan(pi/2 - x) above it: relative error < 1e-6 over the whole range, two divides max.
    static inline float fastTan(float x) {
        constexpr float halfPi = 1.57079632679f;
        constexpr float quarterPi = 0.78539816339f;
        const bool flip = x > quarterPi;
        const float y = flip ? halfPi - x : x;
        const float y2 = y * y;
        const float t = y * (945.0f - y2 * (105.0f - y2)) / (945.0f - y2 * (420.0f - 15.0f * y2));
        return flip ? 1.0f / t : t;
    }

    // Cheap tanh (Pade 3/2), exact 1.0 with zero slope at |x| = 3, so the clamp joins smoothly
    static inline float fastTanh(float x) {
        if (x >= 3.0f) return 1.0f;
        if (x <= -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) * fastRecip(27.0f + 9.0f * x2);
    }

    // h * fastTanh(x / h) (a saturator with headroom h) with the scaling folded into the Pade
    // coefficients: the same curve, two multiplies fewer. h should be a compile-time constant.
    static inline float fastTanhScaled(float x, float h) {
        if (x >= 3.0f * h) return h;
        if (x <= -3.0f * h) return -h;
        const float x2 = x * x;
        const float c = 27.0f * h * h;
        return x * (c + x2) * fastRecip(c + 9.0f * x2);
    }

    // Output safety limiter: exactly linear below the knee, tanh-shaped above, never exceeds 1.0
    static inline float outputLimiter(float x) {
        constexpr float knee = 0.6f;
        constexpr float span = 1.0f - knee;
        if (x > knee)  return knee + span * fastTanh((x - knee) / span);
        if (x < -knee) return -knee - span * fastTanh((-x - knee) / span);
        return x;
    }

    // Map the resonance Q parameter (0.5..20+, as stored in presets and SysEx) onto 4-pole ladder
    // feedback k. Uses the same square-root curve as the resonance knob so knob travel feels linear;
    // k >= 4 (self-oscillation) is reached in the top ~10% of the range.
    static inline float resonanceQToLadderK(float q) {
        float norm = std::sqrt(std::clamp((q - 0.5f) / 19.5f, 0.0f, 1.0f));
        return 4.2f * norm;
    }
};

} // namespace Synth
