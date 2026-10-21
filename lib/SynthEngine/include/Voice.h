#pragma once

#include "Oscillator.h"
#include "Filter.h"
#include "Envelope.h"
#include "Parameters.h"
#include "SineLut.h"
#include <cstdint>
#include <vector>
#include <array>

namespace Synth {

struct Opx4Operator {
    uint32_t phase{0};
    uint32_t phaseInc{0};
    float ratio{1.0f};
    float level{1.0f};
    float detuneHz{0.0f};
};

class Opx4Engine {
public:
    Opx4Engine();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void setAlgorithm(int algo);
    int getAlgorithm() const { return algorithm_; }

    void setFrequency(float freqHz);
    float getFrequency() const { return baseFreqHz_; }

    void setOpRatio(int op, float ratio);
    float getOpRatio(int op) const { return ops_[op & 3].ratio; }

    void setOpLevel(int op, float level);
    float getOpLevel(int op) const { return ops_[op & 3].level; }

    void setOpDetuneHz(int op, float detuneHz);
    float getOpDetuneHz(int op) const { return ops_[op & 3].detuneHz; }

    void setFeedback(float fb) { feedbackIndex_ = fb; }
    float getFeedback() const { return feedbackIndex_; }

    Opx4Operator& getOp(int op) { return ops_[op & 3]; }
    const Opx4Operator& getOp(int op) const { return ops_[op & 3]; }

    float processSample();
    void renderBlock(float* out, int n);
    void renderBlockAdd(float* acc, int n);

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float baseFreqHz_{440.0f};
    int algorithm_{0};
    float feedbackIndex_{0.0f};
    float fbZ1_{0.0f};
    float fbZ2_{0.0f};
    std::array<Opx4Operator, 4> ops_{};

    void updatePhaseIncs();
    inline float computeOp(int opIdx, float modInRadians);
};

using Opx4Voice = Opx4Engine;

class StagePianoEngine {
public:
    StagePianoEngine();
    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void noteOn(uint8_t noteNumber, uint8_t velocity);
    void noteOff();

    float processSample();
    void renderBlock(float* out, int n);
    void renderBlockAdd(float* acc, int n);

    bool isActive() const { return active_; }

    static constexpr size_t DELAY_BUFFER_SIZE = 2048;

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    bool active_{false};
    uint8_t note_{60};
    float velocityNorm_{1.0f};

    std::array<float, DELAY_BUFFER_SIZE> delayLine_{};
    size_t writePos_{0};
    int delayM_{100};

    float thiranA_{0.0f};
    float thiranX1_{0.0f};
    float thiranY1_{0.0f};

    float dispC_{-0.015f};
    float dispX1_{0.0f};
    float dispY1_{0.0f};

    float dampD_{0.2f};
    float dampPrev_{0.0f};
    float loopGain_{0.9996f};

    std::array<float, 128> hammerBuffer_{};
    size_t hammerLen_{0};
    size_t hammerPos_{0};

    float releaseGain_{1.0f};
    float releaseCoef_{0.995f};

    void updateTuning(float freqHz, float velNorm);
};

class StageEpEngine {
public:
    StageEpEngine();
    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void noteOn(uint8_t noteNumber, uint8_t velocity);
    void noteOff();

    void setPhaserRate(float hz) { phaserRateHz_ = hz; }
    void setTremoloDepth(float depth) { tremoloDepth_ = depth; }

    float processSample();
    void processStereo(float& outL, float& outR);
    void renderBlock(float* out, int n);
    void renderBlockStereo(float* left, float* right, int n);
    void renderBlockAdd(float* acc, int n);

    bool isActive() const { return ampEnv_.isActive(); }

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float freqHz_{440.0f};
    float carrierPhase_{0.0f};
    float modPhase_{0.0f};
    float carrierInc_{0.01f};
    float modInc_{0.14f};

    float modEnv_{0.0f};
    float modDecayCoef_{0.995f};
    float fmIndex_{2.0f};
    float drive_{1.5f};

    Envelope ampEnv_;

    float phaserPhase_{0.0f};
    float phaserRateHz_{0.5f};
    std::array<float, 4> apX1_{};
    std::array<float, 4> apY1_{};

    float tremoloPhase_{0.0f};
    float tremoloRateHz_{4.5f};
    float tremoloDepth_{0.5f};
};

class StudioStringsEngine {
public:
    StudioStringsEngine();
    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void noteOn(uint8_t noteNumber, uint8_t velocity);
    void noteOff();

    void setDetuneCents(float cents);

    float processSample();
    void renderBlock(float* out, int n);
    void renderBlockAdd(float* acc, int n);

    bool isActive() const { return ampEnv_.isActive(); }

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float freqHz_{440.0f};
    float detuneCents_{8.0f};

    float phase1_{0.0f};
    float phase2_{0.0f};
    float inc1_{0.01f};
    float inc2_{0.01f};

    uint32_t rng_{0x55AA1234u};
    float noisePrev_{0.0f};
    float velNorm_{1.0f};

    Envelope ampEnv_;

    StateVariableFilter formant1_;
    StateVariableFilter formant2_;
    StateVariableFilter formant3_;

    void updateIncs();
};

class FabricXlEngine {
public:
    FabricXlEngine();
    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void noteOn(uint8_t noteNumber, uint8_t velocity);
    void noteOff();

    void setDetuneCents(float cents);

    float processSample();
    void renderBlock(float* out, int n);
    void renderBlockAdd(float* acc, int n);

    bool isActive() const { return ampEnv_.isActive(); }

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float freqHz_{440.0f};
    float detuneCents_{6.0f};

    float wtPhase1_{0.0f};
    float wtPhase2_{0.0f};
    float inc1_{0.01f};
    float inc2_{0.01f};
    int wtLevel1_{0};
    int wtLevel2_{0};

    float chimePhase_{0.0f};
    float chimeInc_{0.1f};
    float chimeEnv_{0.0f};
    float chimeDecay_{0.998f};

    float breathEnv_{0.0f};
    float breathDecay_{0.996f};
    uint32_t rng_{0x98765432u};

    Envelope ampEnv_;

    void updateIncs();
};

class MarianaEngine {
public:
    MarianaEngine();
    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void noteOn(uint8_t noteNumber, uint8_t velocity);
    void noteOff();

    void setFrequency(float freqHz);
    void setDrive(float drive) { drive_ = drive; }

    float processSample();
    void renderBlock(float* out, int n);
    void renderBlockAdd(float* acc, int n);
    void renderPaths(float* subOut, float* midOut, int n);

    bool isActive() const { return ampEnv_.isActive(); }

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float freqHz_{120.0f};
    float phase_{0.0f};
    float inc_{0.01f};
    float drive_{1.5f};

    StateVariableFilter midHpf_;
    float dcZ1_{0.0f};
    float dcIn1_{0.0f};

    Envelope ampEnv_;
};

class HypeEngine {
public:
    HypeEngine();
    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void noteOn(uint8_t noteNumber, uint8_t velocity);
    void noteOff();

    void setDetuneCents(float cents);
    void setStereoSpread(float spread) { stereoSpread_ = spread; }

    float processSample();
    void processStereo(float& outL, float& outR);
    void renderBlock(float* out, int n);
    void renderBlockStereo(float* left, float* right, int n);
    void renderBlockAdd(float* acc, int n);

    bool isActive() const { return ampEnv_.isActive(); }

    static constexpr size_t STEREO_BUFFER_SIZE = 1024;

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float freqHz_{440.0f};
    float detuneCents_{12.0f};
    float stereoSpread_{0.8f};

    float phase1_{0.0f};
    float phase2_{0.0f};
    float inc1_{0.01f};
    float inc2_{0.01f};

    uint32_t rng_{0xDEADBEEFu};

    std::array<float, STEREO_BUFFER_SIZE> stereoBuf_{};
    size_t stereoWritePos_{0};
    float lfoPhase_{0.0f};

    Envelope ampEnv_;

    void updateIncs();
};

void apply_hype_pump(float* buf_l, float* buf_r, int frames, float pump_depth, float* pump_phase, float pump_inc);

class Voice {
public:
    Voice();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);

    // Voice lifecycle
    void noteOn(uint8_t noteNumber, uint8_t velocity, uint32_t timestamp);
    void legatoNoteChange(uint8_t noteNumber, uint8_t velocity, bool glide);
    void noteOff();
    void releaseWithSustain(bool sustainPedal);
    void releaseSustainPedal();
    // Short fade-out regardless of the release setting (mode / engine switches)
    void fastRelease();
    void kill();

    // Render n samples and ADD them into acc[0..n). bend[i] is the pitch bend (semitones, vibrato
    // included) in effect at sample i; it is read at the control-rate ticks, and bend[n-1] is kept
    // as the voice's pitch bend afterwards. Branches on engine mode, filter model, waveform, drive and
    // sub level are resolved once per block, and state lives in locals across the sample loop.
    void renderBlock(float* acc, int n, const float* bend);

    // Single-sample convenience wrapper around renderBlock() using the last pitch bend (tests, tools)
    float process();

    // Voice status queries
    bool isActive() const { return ampEnv_.isActive(); }
    bool isReleasing() const { return ampEnv_.getState() == EnvelopeState::Release; }
    bool isSustainPedalHeld() const { return sustainPedalHeld_; }
    uint8_t getNoteNumber() const { return noteNumber_; }
    void setRawNoteNumber(uint8_t rawNote) { rawNoteNumber_ = rawNote; }
    uint8_t getRawNoteNumber() const { return rawNoteNumber_; }
    uint32_t getAge() const { return ageTimestamp_; }
    float getCurrentFreqHz() const { return noteToHz(currentNote_); }
    float getTargetFreqHz() const { return noteToHz(targetNote_); }
    float getEnvelopeLevel() const { return ampEnv_.getCurrentValue(); }
    EnvelopeState getAmpEnvelopeState() const { return ampEnv_.getState(); }
    // Filter cutoff after envelope, key tracking and accent (updated at control rate)
    float getEffectiveCutoffHz() const { return effectiveCutoffHz_; }
    bool isAccented() const { return accent_; }

    // Engine Mode (Subtractive vs FM)
    void setEngineMode(EngineMode mode);
    EngineMode getEngineMode() const { return engineMode_; }

    // Portamento / Glide (exponential lag on pitch in semitones, like an RC on a 1 V/oct CV)
    void setGlideTimeMs(float ms);
    float getGlideTimeMs() const { return glideTimeMs_; }
    void setPortamentoEnabled(bool enabled) { portamentoEnabled_ = enabled; }
    bool isPortamentoEnabled() const { return portamentoEnabled_; }

    // Per-model voicing
    void setFilterModel(FilterModel model);
    void setKeyTracking(float amount) { keyTrack_ = amount; }   // 0 = none, 1 = cutoff follows pitch
    void setVelocityToAmp(float amount) { velToAmp_ = amount; } // 0 = no velocity response
    void setAccentEnabled(bool enabled) { accentEnabled_ = enabled; }
    void setAccentAmount(float amount) { accentAmount_ = amount; }
    void setOscKeySync(bool sync) { oscKeySync_ = sync; }
    void setAnalogDrift(float amount) { drift_ = amount; }
    void setSubModMode(SubModMode mode) { subModMode_ = mode; }
    SubModMode getSubModMode() const { return subModMode_; }

    // Subtractive Parameter setters
    void setWaveform(Waveform wave);
    void setBaseCutoffHz(float cutoffHz);
    void setResonanceQ(float q);
    void setFilterEnvAmount(float amount); // 0.0 to 1.0
    void setPitchBend(float semitones);    // includes vibrato; applied at control rate
    void setPulseWidth(float pw);
    void setSubOscLevel(float lvl);       // 0.0 to 1.0 (sub-osc 1 octave down)
    void setDetuneCents(float cents);     // detune osc2 against osc1
    void setDrive(float drive);           // saturation overdrive 1.0 = clean, >1.0 = drive

    // FM Parameter setters
    void setModRatio(float ratio) { modRatio_ = ratio; controlSubsample_ = 0; }
    float getModRatio() const { return modRatio_; }
    void setModDepth(float depth) { modDepth_ = depth; }
    float getModDepth() const { return modDepth_; }
    void setModFeedback(float fb) { modFeedback_ = fb; }
    float getModFeedback() const { return modFeedback_; }

    // Wavetable / phase distortion / pluck / additive parameters
    void setTimbre(const TimbreParams& timbre);
    const TimbreParams& getTimbre() const { return timbre_; }

    // Envelope setters (Carrier = AmpEnv, Modulator = FilterEnv)
    void setAmpAttackMs(float ms);
    void setAmpDecayMs(float ms);
    void setAmpSustainLevel(float level);
    void setAmpReleaseMs(float ms);

    void setFilterAttackMs(float ms);
    void setFilterDecayMs(float ms);
    void setFilterSustainLevel(float level);
    void setFilterReleaseMs(float ms);

    // Control-rate interval (samples) for pitch and filter coefficient updates
    static constexpr uint8_t CONTROL_INTERVAL = 8;

    // Pluck delay line (samples, power of two). The lowest notes that do not fit play an octave up:
    // 1024 holds periods down to 43 Hz (F1) at 44.1 kHz.
    static constexpr int PLUCK_LINE_SIZE = 2048;

private:
    static float noteToHz(float note) { return 440.0f * ParameterScaler::fastExp2((note - 69.0f) * (1.0f / 12.0f)); }
    float nextRandom(); // uniform [-1, 1)

    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    uint8_t noteNumber_{0};
    uint8_t rawNoteNumber_{0};
    uint8_t controlSubsample_{0};
    float velocity_{1.0f};
    uint32_t ageTimestamp_{0};
    EngineMode engineMode_{EngineMode::Subtractive};
    bool sustainPedalHeld_{false};
    uint32_t rng_{0x12345678u};

    // Subtractive Oscillators
    Oscillator osc1_;
    Oscillator osc2_;   // Detuned second oscillator
    Oscillator subOsc_; // Square one octave down, phase-locked to osc1
    bool  subParity_{false};
    float pitchBendSemitones_{0.0f};
    float currentNote_{69.0f};  // fractional MIDI note (glide state)
    float targetNote_{69.0f};
    float glideTimeMs_{0.0f};
    bool  portamentoEnabled_{false};
    float glideAlpha_{1.0f};
    float detuneCents_{8.0f};
    float detuneRatio_{1.0f};
    float subOscLevel_{0.0f};
    float drive_{1.0f};

    // Voicing
    FilterModel filterModel_{FilterModel::Svf12};
    float keyTrack_{0.0f};
    float velToAmp_{1.0f};
    bool  accentEnabled_{false};
    float accentAmount_{0.5f};
    bool  accent_{false};
    float accentCap_{0.0f};     // TB-303 accent sweep capacitor
    float accentCoef_{0.0f};
    bool  oscKeySync_{false};
    float drift_{0.0f};
    float driftCents_{0.0f};    // per-note pitch offset (both oscillators)
    float driftCents2_{0.0f};   // extra per-note offset on osc2
    float driftCutoff_{1.0f};   // per-note cutoff multiplier

    // FM Operator DSP
    float modRatio_{1.0f};
    float modDepth_{2.0f};
    float modFeedback_{0.0f};
    float carrierPhase_{0.0f};
    float carrierPhaseInc_{0.0f};
    float modPhase_{0.0f};
    float modPhaseInc_{0.0f};
    float lastMod1_{0.0f};
    float lastMod2_{0.0f};
    float velocityModScale_{1.0f};
    float keyScale_{1.0f};      // FM keyboard rate scaling applied to envelope decays
    float baseAmpDecayMs_{350.0f};
    float baseFilterDecayMs_{400.0f};

    // Filter
    StateVariableFilter svf_;
    LadderFilter ladder_;
    float baseCutoffHz_{1000.0f};
    float resonanceQ_{1.0f};
    float filterEnvAmount_{0.5f};
    float effectiveCutoffHz_{1000.0f};
    float filterG_{0.1f};
    float filterGStep_{0.0f};
    bool  snapFilter_{true};

    // Envelopes (ampEnv_ = Carrier, filterEnv_ = Modulator)
    Envelope ampEnv_;
    Envelope filterEnv_;

    // Wavetable engine
    float wtPhase1_{0.0f};
    float wtPhase2_{0.0f};
    float wtInc1_{0.0f};
    float wtInc2_{0.0f};
    int   wtLevel_{0};
    float wtPos_{0.0f};        // frame position 0..FRAMES-1, ramped per sample
    float wtPosStep_{0.0f};
    bool  snapWtPos_{true};

    // Phase distortion engine (two CZ "lines", the second detuned)
    float pdPhase1_{0.0f};
    float pdPhase2_{0.0f};
    float pdInc1_{0.0f};
    float pdInc2_{0.0f};
    float pdK1_{1.0f};         // warp slopes, set from the DCW amount at control rate
    float pdK2_{1.0f};
    float pdKnee_{0.5f};
    float pdAmount_{0.0f};     // DCW amount after envelope and key follow
    float pdResoRatio_{1.0f};  // resonance waves: burst frequency / fundamental
    float dcX1_{0.0f};         // DC blocker (phase distortion, additive)
    float dcY1_{0.0f};
    float dcCoef_{0.9986f};

    // Pluck engine (Karplus-Strong with an allpass tuning stage and one-zero loop filter)
    std::vector<float> pluckLine_;
    int   pluckPos_{0};
    int   pluckDelay_{100};    // integer part of the loop delay
    float pluckApCoef_{0.0f};  // fractional-delay allpass
    float pluckApX1_{0.0f};
    float pluckApY1_{0.0f};
    float pluckLpPrev_{0.0f};
    float pluckDamp_{0.25f};   // loop filter: y = (1 - d) x[n] + d x[n-1]
    float pluckGain_{0.99f};   // loop gain per period (sets the T60)
    // Re-tunes the loop for the note and fills the next period of the line with a shaped noise burst
    void  excitePluck(bool fromSilence, float velNorm);

    // Additive engine: harmonic h of the 16' fundamental, h = 1..16; drawbars feed h = 1,3,2,4,6,8,10,12,16
    static constexpr int ADDITIVE_HARMONICS = 16;
    float addPhase_{0.0f};
    float addInc_{0.0f};
    float addBaseWeights_[ADDITIVE_HARMONICS + 1]{}; // from the registration (setTimbre)
    float addWeights_[ADDITIVE_HARMONICS + 1]{};     // band-limited for the current pitch
    float addPercWeight_{0.0f};
    int   addPercHarmonic_{6};
    float clickEnv_{0.0f};
    float clickDecay_{0.99f};
    float clickPrevNoise_{0.0f};

    TimbreParams timbre_{};

    template <Waveform W, bool Ladder, bool Drive, bool Sub>
    void renderSubtractive(float* acc, int n, const float* bend);
    template <bool Ladder, bool Drive>
    void renderWavetable(float* acc, int n, const float* bend);
    template <PdWave W, bool Dual>
    void renderPhaseDist(float* acc, int n, const float* bend);
    template <bool Dual>
    void renderPhaseDistWave(float* acc, int n, const float* bend); // dispatches on timbre_.pdWave
    template <bool Drive>
    void renderPluck(float* acc, int n, const float* bend);
    void renderAdditive(float* acc, int n, const float* bend);
    template <bool Drive>
    void renderFm(float* acc, int n, const float* bend);
    template <Waveform W>
    void renderSubtractiveWave(float* acc, int n, const float* bend);

    // Sub-mod modes (Odyssey, VintageJup, TubeSynth, SubFactory)
    SubModMode subModMode_{SubModMode::None};
    PitchEnvelope808 pitchEnv808_;
    float syncMasterPhase_{0.0f};
    float syncSlavePhase_{0.0f};
    float ringDcZ1_{0.0f};
    float ringDcIn1_{0.0f};
    float tubeDcZ1_{0.0f};
    float tubeDcIn1_{0.0f};

    // Direct engine instances
    Opx4Engine opx4_;
    StageEpEngine stageEp_;
    StudioStringsEngine strings_;
    FabricXlEngine fabric_;
    MarianaEngine mariana_;

    // Waveguide piano state (reusing pluckLine_)
    int   pianoDelayM_{100};
    float thiranA_{0.0f};
    float thiranX1_{0.0f};
    float thiranY1_{0.0f};
    float dispC_{-0.015f};
    float dispX1_{0.0f};
    float dispY1_{0.0f};
    float dampD_{0.2f};
    float dampPrev_{0.0f};
    float loopGain_{0.9996f};
    std::array<float, 128> hammerBuffer_{};
    size_t hammerLen_{0};
    size_t hammerPos_{0};
    float pianoReleaseGain_{1.0f};
    float pianoReleaseCoef_{0.995f};
    size_t pianoWritePos_{0};
    bool pianoActive_{false};
    void initStagePiano(uint8_t noteNumber, uint8_t velocity);

    // Hype hypersaw state (reusing pluckLine_)
    size_t hypeWritePos_{0};
    float  hypeLfoPhase_{0.0f};

    void renderOpx4(float* acc, int n, const float* bend);
    void renderStagePiano(float* acc, int n, const float* bend);
    void renderStageEp(float* acc, int n, const float* bend);
    void renderStudioStrings(float* acc, int n, const float* bend);
    void renderFabricXl(float* acc, int n, const float* bend);
    void renderMariana(float* acc, int n, const float* bend);
    void renderHype(float* acc, int n, const float* bend);
};



} // namespace Synth
