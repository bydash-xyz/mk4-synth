#include "Voice.h"
#include "Wavetables.h"
#include <algorithm>
#include <cmath>

namespace Synth {

Voice::Voice() {
    // Distinct random stream per voice so free-running phases and drift differ between voices
    static uint32_t s_seed = 0x9E3779B9u;
    s_seed = s_seed * 1664525u + 1013904223u;
    rng_ = s_seed | 1u;

    subOsc_.setWaveform(Waveform::Square);
    svf_.setCutoff(baseCutoffHz_);
    svf_.setResonance(1.0f);
    ladder_.setResonance(1.0f);
}

float Voice::nextRandom() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

void Voice::init(float sampleRate) {
    sampleRate_ = sampleRate;
    SineLut::init();
    osc1_.init(sampleRate);
    osc2_.init(sampleRate);
    subOsc_.init(sampleRate);
    subOsc_.setWaveform(Waveform::Square);
    // Free-running oscillators start at arbitrary phases, as analog VCOs do at power-on
    osc1_.resetPhase(0.5f + 0.5f * nextRandom());
    osc2_.resetPhase(0.5f + 0.5f * nextRandom());
    svf_.init(sampleRate);
    ladder_.init(sampleRate);
    ampEnv_.init(sampleRate);
    filterEnv_.init(sampleRate);
    accentCoef_ = 1.0f - std::exp(-1.0f / (0.040f * sampleRate)); // 40 ms accent sweep RC
    Wavetables::init();
    if (pluckLine_.empty()) pluckLine_.assign(PLUCK_LINE_SIZE, 0.0f);
    // Arbitrary start phases hashed from the voice's seed, leaving its random stream (drift) untouched.
    // Tonewheels are always spinning, so additive voices never restart their phase.
    uint32_t h = rng_ * 2654435761u;
    wtPhase1_ = static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    h = h * 1664525u + 1013904223u;
    wtPhase2_ = static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    h = h * 1664525u + 1013904223u;
    addPhase_ = static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    pitchEnv808_.init(sampleRate);
    opx4_.init(sampleRate);
    stageEp_.init(sampleRate);
    strings_.init(sampleRate);
    fabric_.init(sampleRate);
    mariana_.init(sampleRate);
    setSampleRate(sampleRate);
}

void Voice::setSampleRate(float sampleRate) {
    sampleRate_ = sampleRate;
    osc1_.setSampleRate(sampleRate);
    osc2_.setSampleRate(sampleRate);
    subOsc_.setSampleRate(sampleRate);
    svf_.setSampleRate(sampleRate);
    ladder_.setSampleRate(sampleRate);
    ampEnv_.setSampleRate(sampleRate);
    filterEnv_.setSampleRate(sampleRate);
    accentCoef_ = 1.0f - std::exp(-1.0f / (0.040f * sampleRate));
    dcCoef_ = 1.0f - TWO_PI_F * 10.0f / sampleRate;           // DC blocker corner ~10 Hz
    clickDecay_ = std::exp(-1.0f / (0.0025f * sampleRate));   // 2.5 ms key click
    setGlideTimeMs(glideTimeMs_);
    controlSubsample_ = 0;
    snapFilter_ = true;
    snapWtPos_ = true;
}

void Voice::setGlideTimeMs(float ms) {
    glideTimeMs_ = std::clamp(ms, 0.0f, 2000.0f);
    if (glideTimeMs_ > 0.5f && sampleRate_ > 0.0f) {
        glideAlpha_ = 1.0f - std::exp(-1000.0f / (glideTimeMs_ * sampleRate_));
    } else {
        glideAlpha_ = 1.0f;
    }
}

void Voice::setEngineMode(EngineMode mode) {
    if (engineMode_ != mode) {
        // A voice still fading out under another engine would otherwise replay its last string
        if (mode == EngineMode::Pluck) {
            std::fill(pluckLine_.begin(), pluckLine_.end(), 0.0f);
            pluckApX1_ = pluckApY1_ = pluckLpPrev_ = 0.0f;
        }
        engineMode_ = mode;
        keyScale_ = 1.0f;
        ampEnv_.setDecayMs(baseAmpDecayMs_);
        filterEnv_.setDecayMs(baseFilterDecayMs_);
        controlSubsample_ = 0; // recompute phase increments for the new engine on the next sample
    }
}

void Voice::setFilterModel(FilterModel model) {
    if (filterModel_ != model) {
        filterModel_ = model;
        svf_.reset();
        ladder_.reset();
        snapFilter_ = true;
    }
}

void Voice::noteOn(uint8_t noteNumber, uint8_t velocity, uint32_t timestamp) {
    const bool wasIdle = !ampEnv_.isActive();

    noteNumber_ = noteNumber;
    rawNoteNumber_ = noteNumber;
    ageTimestamp_ = timestamp;
    sustainPedalHeld_ = false;
    controlSubsample_ = 0;

    float velNorm = static_cast<float>(velocity) / 127.0f;
    velocity_ = (1.0f - velToAmp_) + velToAmp_ * (0.15f + 0.85f * (velNorm * velNorm));
    // FM brightness follows velocity partially (DX-style sensitivity), never collapsing to a pure sine
    velocityModScale_ = 0.25f + 0.75f * (velNorm * velNorm);
    accent_ = accentEnabled_ && velocity >= 100;

    targetNote_ = static_cast<float>(noteNumber_);
    if (!portamentoEnabled_ || wasIdle || isReleasing()) {
        currentNote_ = targetNote_;
    }

    // Per-note analog variation
    driftCents_  = drift_ * 3.0f * nextRandom();
    driftCents2_ = drift_ * 2.0f * nextRandom();
    driftCutoff_ = std::exp2(drift_ * 0.05f * nextRandom());

    if (wasIdle) {
        // Starting from silence: no discontinuity is possible, so this is the only place state resets
        svf_.reset();
        ladder_.reset();
        accentCap_ = 0.0f;
        snapFilter_ = true;
        snapWtPos_ = true;
        lastMod1_ = 0.0f;
        lastMod2_ = 0.0f;
        dcX1_ = 0.0f;
        dcY1_ = 0.0f;
        if (oscKeySync_) {
            osc1_.resetPhase(0.0f);
            osc2_.resetPhase(0.0f);
            subParity_ = false;
            carrierPhase_ = 0.0f;
            modPhase_     = 0.0f;
            wtPhase1_ = wtPhase2_ = 0.0f;
            pdPhase1_ = pdPhase2_ = 0.0f;
        }
    }
    if (engineMode_ == EngineMode::Pluck) excitePluck(wasIdle, velNorm);
    if (engineMode_ == EngineMode::Additive) clickEnv_ = timbre_.keyClick * (0.5f + 0.5f * velNorm);

    syncMasterPhase_ = 0.0f;
    syncSlavePhase_ = 0.0f;
    ringDcZ1_ = 0.0f; ringDcIn1_ = 0.0f;
    tubeDcZ1_ = 0.0f; tubeDcIn1_ = 0.0f;
    if (subModMode_ == SubModMode::Sub808) {
        pitchEnv808_.setClickTimeMs(filterEnv_.getAttackMs());
        pitchEnv808_.setBodyTimeMs(filterEnv_.getDecayMs());
        pitchEnv808_.trigger();
    }
    if (engineMode_ == EngineMode::Opx4) {
        opx4_.reset();
        opx4_.setAlgorithm(timbre_.opxAlgo);
        opx4_.setFrequency(noteToHz(currentNote_));
    } else if (engineMode_ == EngineMode::StageEp) {
        stageEp_.noteOn(noteNumber, velocity);
        stageEp_.setTremoloDepth(0.0f);
    } else if (engineMode_ == EngineMode::StudioStrings) {
        strings_.noteOn(noteNumber, velocity);
        strings_.setDetuneCents(detuneCents_);
    } else if (engineMode_ == EngineMode::FabricXl) {
        fabric_.noteOn(noteNumber, velocity);
        fabric_.setDetuneCents(detuneCents_);
    } else if (engineMode_ == EngineMode::Mariana) {
        mariana_.noteOn(noteNumber, velocity);
        mariana_.setFrequency(noteToHz(currentNote_));
        mariana_.setDrive(drive_);
    } else if (engineMode_ == EngineMode::StagePiano) {
        initStagePiano(noteNumber, velocity);
    } else if (engineMode_ == EngineMode::Hype) {
        hypeWritePos_ = 0;
        hypeLfoPhase_ = 0.0f;
    }
    // A sounding voice keeps its oscillator phases and filter state: the envelope continues from its
    // current level, so retriggers and steals are click-free (analog VCOs are free-running).

    if (engineMode_ == EngineMode::FM) {
        // Keyboard rate scaling: higher notes naturally decay faster (acoustic tine / piano behavior)
        keyScale_ = std::clamp(std::pow(2.0f, (60.0f - static_cast<float>(noteNumber_)) / 36.0f), 0.4f, 2.0f);
    } else {
        keyScale_ = 1.0f;
    }
    ampEnv_.setDecayMs(baseAmpDecayMs_ * keyScale_);
    filterEnv_.setDecayMs(baseFilterDecayMs_ * keyScale_);

    ampEnv_.noteOn();
    filterEnv_.noteOn();
}

void Voice::legatoNoteChange(uint8_t noteNumber, uint8_t velocity, bool glide) {
    noteNumber_ = noteNumber;
    rawNoteNumber_ = noteNumber;
    targetNote_ = static_cast<float>(noteNumber_);

    if (!glide || glideTimeMs_ <= 0.5f) {
        currentNote_ = targetNote_;
        controlSubsample_ = 0;
    }

    // Loudness stays with the first note of the phrase (mono analogs ignore velocity on legato),
    // but TB-303 accent is evaluated per step, slides included.
    accent_ = accentEnabled_ && velocity >= 100;

    // Envelopes are intentionally not reset to attack; they continue their current contour
}

void Voice::noteOff() {
    ampEnv_.noteOff();
    filterEnv_.noteOff();
    if (engineMode_ == EngineMode::StageEp) stageEp_.noteOff();
    if (engineMode_ == EngineMode::StudioStrings) strings_.noteOff();
    if (engineMode_ == EngineMode::FabricXl) fabric_.noteOff();
    if (engineMode_ == EngineMode::Mariana) mariana_.noteOff();
    if (engineMode_ == EngineMode::StagePiano) {
        pianoReleaseGain_ = 0.998f;
        pianoReleaseCoef_ = 0.994f;
    }
}

void Voice::fastRelease() {
    sustainPedalHeld_ = false;
    ampEnv_.fastRelease();
    filterEnv_.noteOff();
}

void Voice::releaseWithSustain(bool sustainPedal) {
    if (!ampEnv_.isActive()) return;
    if (sustainPedal) {
        sustainPedalHeld_ = true;
    } else {
        sustainPedalHeld_ = false;
        noteOff();
    }
}

void Voice::releaseSustainPedal() {
    if (sustainPedalHeld_) {
        sustainPedalHeld_ = false;
        if (ampEnv_.isActive()) {
            noteOff();
        }
    }
}

void Voice::kill() {
    sustainPedalHeld_ = false;
    carrierPhase_ = 0.0f;
    modPhase_     = 0.0f;
    lastMod1_     = 0.0f;
    lastMod2_     = 0.0f;
    accentCap_    = 0.0f;
    dcX1_ = dcY1_ = 0.0f;
    clickEnv_     = 0.0f;
    std::fill(pluckLine_.begin(), pluckLine_.end(), 0.0f);
    pluckApX1_ = pluckApY1_ = pluckLpPrev_ = 0.0f;
    ampEnv_.reset();
    filterEnv_.reset();
    svf_.reset();
    ladder_.reset();
    snapFilter_ = true;
}

void Voice::setWaveform(Waveform wave) {
    osc1_.setWaveform(wave);
    osc2_.setWaveform(wave);
}

void Voice::setBaseCutoffHz(float cutoffHz) {
    baseCutoffHz_ = cutoffHz;
    controlSubsample_ = 0;
}

void Voice::setResonanceQ(float q) {
    resonanceQ_ = q;
    svf_.setResonance(q);
    ladder_.setResonance(q);
}

void Voice::setFilterEnvAmount(float amount) {
    filterEnvAmount_ = std::clamp(amount, 0.0f, 1.0f);
}

void Voice::setPitchBend(float semitones) {
    pitchBendSemitones_ = semitones;
}

void Voice::setPulseWidth(float pw) {
    osc1_.setPulseWidth(pw);
    osc2_.setPulseWidth(pw);
}

void Voice::setSubOscLevel(float lvl) {
    subOscLevel_ = std::clamp(lvl, 0.0f, 1.0f);
}

void Voice::setDetuneCents(float cents) {
    if (cents != detuneCents_) {
        detuneCents_ = cents;
        detuneRatio_ = std::exp2(detuneCents_ / 1200.0f);
        controlSubsample_ = 0;
    }
}

void Voice::setDrive(float drive) {
    drive_ = std::clamp(drive, 0.5f, 4.0f);
}

void Voice::setTimbre(const TimbreParams& timbre) {
    timbre_ = timbre;
    timbre_.wtTable = std::min<uint8_t>(timbre_.wtTable, static_cast<uint8_t>(WAVETABLE_COUNT - 1));
    if (static_cast<uint8_t>(timbre_.pdWave) >= static_cast<uint8_t>(PdWave::Count)) timbre_.pdWave = PdWave::Saw;

    // Drawbar registration: each step is 3 dB (a Hammond drawbar's taper), 0 = off. The sum is normalised
    // by its power so registrations play at a similar level; the per-model trim does the rest.
    static const int kDrawbarHarmonic[DRAWBAR_COUNT] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
    float amps[DRAWBAR_COUNT];
    float power = 0.0f;
    for (size_t i = 0; i < DRAWBAR_COUNT; ++i) {
        const float level = std::clamp(timbre_.drawbars[i], 0.0f, 8.0f);
        amps[i] = level <= 0.0f ? 0.0f : std::exp2((level - 8.0f) * 0.5f); // 2^(-0.5) per step = -3 dB
        power += amps[i] * amps[i];
    }
    const float norm = 0.5f / std::sqrt(std::max(power, 0.25f));
    std::fill(std::begin(addBaseWeights_), std::end(addBaseWeights_), 0.0f);
    for (size_t i = 0; i < DRAWBAR_COUNT; ++i) addBaseWeights_[kDrawbarHarmonic[i]] += amps[i] * norm;
    addPercHarmonic_ = timbre_.percHarmonic == 0 ? 4 : 6; // 4' (second) or 2 2/3' (third) of the 8' note
    addPercWeight_   = 0.5f * std::clamp(timbre_.percLevel, 0.0f, 1.0f);
    controlSubsample_ = 0; // recompute the band-limited weights / loop coefficients on the next sample
}

void Voice::setAmpAttackMs(float ms)      { ampEnv_.setAttackMs(ms); }
void Voice::setAmpDecayMs(float ms)       { baseAmpDecayMs_ = ms; ampEnv_.setDecayMs(ms * keyScale_); }
void Voice::setAmpSustainLevel(float lvl) { ampEnv_.setSustainLevel(lvl); }
void Voice::setAmpReleaseMs(float ms)     { ampEnv_.setReleaseMs(ms); }

void Voice::setFilterAttackMs(float ms)      { filterEnv_.setAttackMs(ms); }
void Voice::setFilterDecayMs(float ms)       { baseFilterDecayMs_ = ms; filterEnv_.setDecayMs(ms * keyScale_); }
void Voice::setFilterSustainLevel(float lvl) { filterEnv_.setSustainLevel(lvl); }
void Voice::setFilterReleaseMs(float ms)     { filterEnv_.setReleaseMs(ms); }

// ---------------------------------------------------------------------------
// Block rendering
//
// Per sample, in this order (the same as the former per-sample Voice::process()):
//   1. glide the pitch (semitone domain)   2. advance both envelopes
//   3. TB-303 accent sweep and level       4. control-rate tick every CONTROL_INTERVAL samples:
//      oscillator increments and filter cutoff target (ramped per sample over the interval)
//   5. oscillators -> drive -> filter (or the 2-op FM pair) -> amp
// Loop-invariant choices are template parameters, and per-sample state is held in locals and
// written back once, so the compiler can keep it in registers (stores to acc cannot alias it).
// ---------------------------------------------------------------------------

namespace {
// Filter a subtractive voice runs, selected at compile time
template <bool Ladder> struct VoiceFilterSel { using Type = StateVariableFilter; };
template <> struct VoiceFilterSel<true> { using Type = LadderFilter; };
} // namespace

template <Waveform W, bool Ladder, bool Drive, bool Sub>
void Voice::renderSubtractive(float* acc, int n, const float* bend) {
    using Flt = typename VoiceFilterSel<Ladder>::Type;
    Flt* fltMember;
    if constexpr (Ladder) fltMember = &ladder_; else fltMember = &svf_;

    // Working copies (written back after the loop)
    Oscillator o1 = osc1_;
    Oscillator o2 = osc2_;
    Oscillator so = subOsc_;
    Envelope aEnv = ampEnv_;
    Envelope fEnvGen = filterEnv_;
    Flt flt = *fltMember;
    float cur = currentNote_;
    float accentCap = accentCap_;
    float g = filterG_;
    float gStep = filterGStep_;
    float effCutoff = effectiveCutoffHz_;
    bool snap = snapFilter_;
    bool parity = subParity_;
    int cs = controlSubsample_;

    // Loop invariants
    const float target = targetNote_;
    const float glideAlpha = glideAlpha_;
    const float vel = velocity_;
    const bool accentEnabled = accentEnabled_;
    const bool accent = accent_;
    const float accentCoef = accentCoef_;
    const float accentAmount = accentAmount_;
    const float driftCents = driftCents_;
    const float drift2 = ParameterScaler::fastExp2(driftCents2_ * (1.0f / 1200.0f));
    const float detuneRatio = detuneRatio_;
    const float driftCutoff = driftCutoff_;
    const float baseCutoff = baseCutoffHz_;
    const float envAmount = filterEnvAmount_;
    const float keyTrack = keyTrack_;
    const float sr = sampleRate_;
    const float subGain = subOscLevel_ * 0.5f;
    const float drive = drive_;

    auto oscSample = [](Oscillator& o) -> float {
        if constexpr (W == Waveform::Count) return o.process(); // generic: per-oscillator waveform
        else return o.template processWave<W>();
    };

    for (int i = 0; i < n; ++i) {
        // 1. Pitch glide in the semitone domain (symmetric up/down, like portamento on a 1 V/oct CV)
        if (cur != target) {
            const float diff = target - cur;
            if (std::abs(diff) < 0.001f) {
                cur = target;
            } else {
                cur += diff * glideAlpha;
            }
        }

        // 2. Envelopes
        float amp = aEnv.process() * vel;
        const float fEnv = fEnvGen.process();

        // 3. TB-303 accent: the sweep capacitor charges from the filter envelope on accented steps and
        // bleeds off otherwise, so consecutive accents stack up
        if (accentEnabled) {
            accentCap += ((accent ? fEnv : 0.0f) - accentCap) * accentCoef;
            if (accent) amp *= 1.0f + 0.6f * accentAmount;
        }

        // 4. Control-rate pitch / cutoff update
        float pitchMod808 = 0.0f;
        if (subModMode_ == SubModMode::Sub808) {
            pitchMod808 = pitchEnv808_.process() * envAmount;
        }

        if (cs == 0) {
            const float pitchNote = cur + bend[i] + driftCents * 0.01f + pitchMod808;
            const float freq = noteToHz(pitchNote);
            o1.setFrequency(freq);
            o2.setFrequency(freq * detuneRatio * drift2);
            so.setFrequency(freq * 0.5f);

            // Cutoff: base * envelope * key tracking (C4 pivot) * accent sweep * per-note drift
            float octaves = fEnv * envAmount * 4.0f
                          + keyTrack * (cur - 60.0f) / 12.0f
                          + accentAmount * 3.0f * accentCap;
            effCutoff = std::clamp(baseCutoff * driftCutoff * ParameterScaler::fastExp2(octaves), 10.0f, sr * 0.45f);

            // Ramp the integrator gain across the control interval so fast sweeps don't staircase.
            // The ladder ramps its normalised gain g/(1+g) directly, which saves a division per sample
            float gTarget = cutoffToGFast(effCutoff, sr);
            if constexpr (Ladder) gTarget = gTarget / (1.0f + gTarget);
            if (snap) {
                g = gTarget;
                gStep = 0.0f;
                snap = false;
            } else {
                gStep = (gTarget - g) / static_cast<float>(CONTROL_INTERVAL);
            }
        }
        if (++cs >= CONTROL_INTERVAL) cs = 0;

        // 5. Oscillators. The sub is phase-locked to osc1 (a divide-by-two, as on the Juno DCO); its
        // parity is tracked even while it is silent so turning it up later keeps the same lock.
        float osc;
        if (subModMode_ == SubModMode::HardSyncRing) {
            float slaveRatio = 1.0f + (fEnv * envAmount * 3.0f);
            float masterInc = (noteToHz(cur + bend[i] + driftCents * 0.01f)) / sr;
            float slaveInc = (noteToHz(cur + bend[i] + driftCents * 0.01f) * slaveRatio * detuneRatio) / sr;
            float syncSaw = process_hard_sync_saw(&syncMasterPhase_, masterInc, &syncSlavePhase_, slaveInc);
            float o2Val = oscSample(o2);
            float ring = ring_modulate(syncSaw, o2Val, &ringDcZ1_, &ringDcIn1_);
            osc = syncSaw * (1.0f - subGain) + ring * subGain;
        } else if (subModMode_ == SubModMode::CrossMod) {
            float o2Val = oscSample(o2);
            float o1Inc = (noteToHz(cur + bend[i] + driftCents * 0.01f)) / sr;
            float warpedInc = cross_modulate(o1Inc, o2Val, subGain * 2.0f);
            o1.setFrequency(warpedInc * sr);
            float o1Val = oscSample(o1);
            osc = 0.6f * o1Val + 0.4f * o2Val;
        } else {
            if constexpr (Sub) {
                so.setPhaseOnly(0.5f * (o1.getPhase() + (parity ? 1.0f : 0.0f)));
                osc = oscSample(o1) * 0.6f + oscSample(o2) * 0.4f + so.processWave<Waveform::Square>() * subGain;
            } else {
                osc = oscSample(o1) * 0.6f + oscSample(o2) * 0.4f;
            }
            if (o1.didWrap()) parity = !parity;
        }

        // Overdrive / saturation before the filter
        if (subModMode_ == SubModMode::TubeDrive || subModMode_ == SubModMode::Sub808) {
            osc = tube_saturate_sample(osc, drive, 0.15f, &tubeDcZ1_, &tubeDcIn1_);
        } else if constexpr (Drive) {
            osc = ParameterScaler::softClip(osc * drive);
        }

        g += gStep;
        float filtered;
        if constexpr (Ladder) {
            flt.setNormalizedGain(g);
            filtered = flt.process(osc);
        } else {
            flt.setG(g);
            filtered = flt.processLowpass(osc);
        }

        // Two statements: keeps the product rounded before the sum (no FMA contraction), so the mix
        // is bit-identical to summing per-sample voice outputs
        const float out = filtered * amp;
        acc[i] += out;

        // The voice went idle on this sample: the rest of the block is silent and its state frozen
        if (!aEnv.isActive()) break;
    }

    // The sub-oscillator's phase is re-derived from osc1 every sample, so only its frequency matters
    osc1_ = o1;
    osc2_ = o2;
    subOsc_ = so;
    ampEnv_ = aEnv;
    filterEnv_ = fEnvGen;
    *fltMember = flt;
    currentNote_ = cur;
    accentCap_ = accentCap;
    filterG_ = g;
    filterGStep_ = gStep;
    effectiveCutoffHz_ = effCutoff;
    snapFilter_ = snap;
    subParity_ = parity;
    controlSubsample_ = static_cast<uint8_t>(cs);
}

template <bool Drive>
void Voice::renderFm(float* acc, int n, const float* bend) {
    Envelope aEnv = ampEnv_;
    Envelope fEnvGen = filterEnv_;
    float cur = currentNote_;
    float accentCap = accentCap_;
    float carrierPhase = carrierPhase_;
    float carrierInc = carrierPhaseInc_;
    float modPhase = modPhase_;
    float modInc = modPhaseInc_;
    float lastMod1 = lastMod1_;
    float lastMod2 = lastMod2_;
    int cs = controlSubsample_;

    const float target = targetNote_;
    const float glideAlpha = glideAlpha_;
    const float vel = velocity_;
    const bool accentEnabled = accentEnabled_;
    const bool accent = accent_;
    const float accentCoef = accentCoef_;
    const float accentAmount = accentAmount_;
    const float driftCents = driftCents_;
    const float sr = sampleRate_;
    const float modRatio = modRatio_;
    const float modDepth = modDepth_;
    const float modFeedback = modFeedback_;
    const float velocityModScale = velocityModScale_;
    const float drive = drive_;

    for (int i = 0; i < n; ++i) {
        if (cur != target) {
            const float diff = target - cur;
            if (std::abs(diff) < 0.001f) {
                cur = target;
            } else {
                cur += diff * glideAlpha;
            }
        }

        float amp = aEnv.process() * vel;
        const float fEnv = fEnvGen.process();

        if (accentEnabled) {
            accentCap += ((accent ? fEnv : 0.0f) - accentCap) * accentCoef;
            if (accent) amp *= 1.0f + 0.6f * accentAmount;
        }

        if (cs == 0) {
            const float pitchNote = cur + bend[i] + driftCents * 0.01f;
            const float freq = noteToHz(pitchNote);
            carrierInc = (freq * TWO_PI_F) / sr;
            modInc     = (freq * modRatio * TWO_PI_F) / sr;
        }
        if (++cs >= CONTROL_INTERVAL) cs = 0;

        // Modulator with 2-sample averaged feedback (DX-style self-feedback)
        float feedbackMod = modFeedback * 0.5f * (lastMod1 + lastMod2);
        float modSample = SineLut::lookupFast(modPhase + feedbackMod);
        lastMod2 = lastMod1;
        lastMod1 = modSample;

        modPhase += modInc;
        if (modPhase >= TWO_PI_F) modPhase -= TWO_PI_F;

        // Carrier phase modulation
        float effectiveIndex = modDepth * fEnv * velocityModScale;
        float carrierSample = SineLut::lookupFast(carrierPhase + effectiveIndex * modSample);

        carrierPhase += carrierInc;
        if (carrierPhase >= TWO_PI_F) carrierPhase -= TWO_PI_F;

        if constexpr (Drive) carrierSample = ParameterScaler::softClip(carrierSample * drive);

        const float out = carrierSample * amp;
        acc[i] += out;

        if (!aEnv.isActive()) break;
    }

    ampEnv_ = aEnv;
    filterEnv_ = fEnvGen;
    currentNote_ = cur;
    accentCap_ = accentCap;
    carrierPhase_ = carrierPhase;
    carrierPhaseInc_ = carrierInc;
    modPhase_ = modPhase;
    modPhaseInc_ = modInc;
    lastMod1_ = lastMod1;
    lastMod2_ = lastMod2;
    controlSubsample_ = static_cast<uint8_t>(cs);
}

template <Waveform W>
void Voice::renderSubtractiveWave(float* acc, int n, const float* bend) {
    const bool ladder = filterModel_ == FilterModel::Ladder24;
    const bool drive = drive_ > 1.0f;
    // Waveform::Count is the generic fallback and always renders the sub term
    const bool sub = (W == Waveform::Count) || subOscLevel_ > 0.0f;
    if (ladder) {
        if (drive) sub ? renderSubtractive<W, true, true, true>(acc, n, bend)
                       : renderSubtractive<W, true, true, false>(acc, n, bend);
        else       sub ? renderSubtractive<W, true, false, true>(acc, n, bend)
                       : renderSubtractive<W, true, false, false>(acc, n, bend);
    } else {
        if (drive) sub ? renderSubtractive<W, false, true, true>(acc, n, bend)
                       : renderSubtractive<W, false, true, false>(acc, n, bend);
        else       sub ? renderSubtractive<W, false, false, true>(acc, n, bend)
                       : renderSubtractive<W, false, false, false>(acc, n, bend);
    }
}

void Voice::renderBlock(float* acc, int n, const float* bend) {
    if (n <= 0) return;
    if (ampEnv_.isActive()) {
        const bool drive = drive_ > 1.0f;
        if (engineMode_ == EngineMode::FM) {
            if (drive) renderFm<true>(acc, n, bend);
            else       renderFm<false>(acc, n, bend);
        } else if (engineMode_ == EngineMode::Wavetable) {
            const bool ladder = filterModel_ == FilterModel::Ladder24;
            if (ladder) drive ? renderWavetable<true, true>(acc, n, bend) : renderWavetable<true, false>(acc, n, bend);
            else        drive ? renderWavetable<false, true>(acc, n, bend) : renderWavetable<false, false>(acc, n, bend);
        } else if (engineMode_ == EngineMode::PhaseDist) {
            if (detuneCents_ > 0.0f) renderPhaseDistWave<true>(acc, n, bend);
            else                     renderPhaseDistWave<false>(acc, n, bend);
        } else if (engineMode_ == EngineMode::Pluck) {
            if (drive) renderPluck<true>(acc, n, bend);
            else       renderPluck<false>(acc, n, bend);
        } else if (engineMode_ == EngineMode::Additive) {
            renderAdditive(acc, n, bend);
        } else if (engineMode_ == EngineMode::Opx4) {
            renderOpx4(acc, n, bend);
        } else if (engineMode_ == EngineMode::StagePiano) {
            renderStagePiano(acc, n, bend);
        } else if (engineMode_ == EngineMode::StageEp) {
            renderStageEp(acc, n, bend);
        } else if (engineMode_ == EngineMode::StudioStrings) {
            renderStudioStrings(acc, n, bend);
        } else if (engineMode_ == EngineMode::FabricXl) {
            renderFabricXl(acc, n, bend);
        } else if (engineMode_ == EngineMode::Mariana) {
            renderMariana(acc, n, bend);
        } else if (engineMode_ == EngineMode::Hype) {
            renderHype(acc, n, bend);
        } else {
            const Waveform w = osc1_.getWaveform();
            if (w != osc2_.getWaveform()) {
                renderSubtractiveWave<Waveform::Count>(acc, n, bend);
            } else {
                switch (w) {
                    case Waveform::Sawtooth: renderSubtractiveWave<Waveform::Sawtooth>(acc, n, bend); break;
                    case Waveform::Square:   renderSubtractiveWave<Waveform::Square>(acc, n, bend); break;
                    case Waveform::Triangle: renderSubtractiveWave<Waveform::Triangle>(acc, n, bend); break;
                    default:                 renderSubtractiveWave<Waveform::Count>(acc, n, bend); break;
                }
            }
        }
    }
    pitchBendSemitones_ = bend[n - 1];
}

float Voice::process() {
    float out = 0.0f;
    const float bend = pitchBendSemitones_;
    renderBlock(&out, 1, &bend);
    return out;
}

} // namespace Synth
