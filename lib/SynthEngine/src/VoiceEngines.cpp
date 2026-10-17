// Block renderers of the wavetable, phase distortion, pluck and additive engines. Same structure as the
// subtractive and FM renderers in Voice.cpp: per-sample glide and envelopes, a control-rate tick every
// CONTROL_INTERVAL samples for pitch-dependent coefficients, state held in locals across the loop and
// written back once, so a block renders the same as the same samples rendered one at a time.

#include "Voice.h"
#include "Wavetables.h"
#include <algorithm>
#include <cmath>

namespace Synth {

namespace {

template <bool Ladder> struct EngineFilterSel { using Type = StateVariableFilter; };
template <> struct EngineFilterSel<true> { using Type = LadderFilter; };

constexpr float kHalfPi = 1.57079632679f;
constexpr int   kPluckMask = Voice::PLUCK_LINE_SIZE - 1;

inline void glideStep(float& cur, float target, float alpha) {
    if (cur != target) {
        const float diff = target - cur;
        if (std::abs(diff) < 0.001f) cur = target;
        else cur += diff * alpha;
    }
}

inline float cosCycles(float w) { return SineLut::lookupFast(TWO_PI_F * w + kHalfPi); }

// ---- Phase distortion -----------------------------------------------------------------------------

// Warp coefficients of one DCW amount (0 = pure cosine), recomputed at control rate
struct PdShape {
    float knee{0.5f}, k1{1.0f}, k2{1.0f}, amount{0.0f}, reso{1.0f};
};

// One CZ line at phase p (cycles, [0, 1)). Saw / square / pulse / double sine bend the phase that reads a
// cosine; the resonance waves read a sine burst at `reso` x the fundamental, restarted every cycle under
// a window that is zero where the burst restarts, so the waveform stays continuous.
template <PdWave W>
inline float pdSample(float p, const PdShape& s) {
    if constexpr (W == PdWave::Saw) {
        return cosCycles(p < s.knee ? p * s.k1 : 0.5f + (p - s.knee) * s.k2);
    } else if constexpr (W == PdWave::Square) {
        const bool second = p >= 0.5f;
        const float q = second ? p - 0.5f : p;
        return cosCycles((second ? 0.5f : 0.0f) + std::min(q * s.k1, 0.5f));
    } else if constexpr (W == PdWave::Pulse) {
        return cosCycles(std::min(p * s.k1, 1.0f)) - s.amount; // held at +1 for `amount` of the cycle
    } else if constexpr (W == PdWave::DoubleSine) {
        const float q = p < 0.5f ? 2.0f * p : 2.0f * p - 1.0f;
        return cosCycles(p + s.amount * (q - p));
    } else {
        float win;
        if constexpr (W == PdWave::ResoSaw)      win = 1.0f - p;
        else if constexpr (W == PdWave::ResoTri) win = p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p;
        else                                     win = std::min(1.0f, 2.0f - 2.0f * p);
        return win * (1.0f - cosCycles(s.reso * p)) - 0.5f;
    }
}

PdShape pdShape(PdWave wave, float amount, float inc) {
    PdShape s;
    s.amount = amount;
    switch (wave) {
        case PdWave::Saw:
            s.knee = 0.5f - 0.5f * amount;
            s.k1 = 0.5f / s.knee;
            s.k2 = 0.5f / (1.0f - s.knee);
            break;
        case PdWave::Square:
        case PdWave::Pulse:
            s.k1 = 1.0f / (1.0f - amount);
            break;
        case PdWave::ResoSaw:
        case PdWave::ResoTri:
        case PdWave::ResoTrap:
            // Keep the burst below a quarter of the sample rate: the window's sidebands need the room
            s.reso = std::min(1.0f + amount * 14.0f, std::max(1.0f, 0.25f / std::max(inc, 1.0e-6f)));
            break;
        default:
            break;
    }
    return s;
}

// ---- Pluck ------------------------------------------------------------------------------------------

struct PluckLoop {
    int   delay;  // integer delay-line length
    float apCoef; // fractional-delay allpass coefficient
    float gain;   // loop gain per pass
};

// Loop tuning: line + allpass + loop filter (about `damp` samples of delay) = one period. The allpass
// carries 0.5..1.5 samples so its coefficient stays small and its delay flat. Periods too long for the
// line play an octave up. The gain makes the fundamental fall 60 dB in decayMs.
PluckLoop pluckLoop(float freqHz, float sr, float damp, float decayMs) {
    float period = sr / std::max(freqHz, 1.0f);
    while (period > static_cast<float>(Voice::PLUCK_LINE_SIZE - 4)) period *= 0.5f;
    period = std::max(period, 4.0f);
    const float d = period - damp;
    int   delay = static_cast<int>(d);
    float frac  = d - static_cast<float>(delay);
    if (frac < 0.5f) { delay -= 1; frac += 1.0f; }
    PluckLoop loop;
    loop.delay  = delay;
    loop.apCoef = (1.0f - frac) / (1.0f + frac);
    const float passesPerSec = sr / period;
    loop.gain = ParameterScaler::fastExp2(-9.9657843f / (passesPerSec * std::max(decayMs, 10.0f) * 0.001f));
    return loop;
}

inline float pluckDamping(float bright) { return 0.05f + 0.45f * (1.0f - std::clamp(bright, 0.0f, 1.0f)); }

} // namespace

// -------------------------------------------------------------------------------------------------------
// Wavetable: two detuned table oscillators morphing across frames -> drive -> filter -> amp. The filter
// envelope sweeps both the cutoff (filterEnvAmount) and the frame position (wtEnvAmount), as the PPG's
// envelope 2 swept the wave.
// -------------------------------------------------------------------------------------------------------
template <bool Ladder, bool Drive>
void Voice::renderWavetable(float* acc, int n, const float* bend) {
    using Flt = typename EngineFilterSel<Ladder>::Type;
    Flt* fltMember;
    if constexpr (Ladder) fltMember = &ladder_; else fltMember = &svf_;

    Envelope aEnv = ampEnv_;
    Envelope fEnvGen = filterEnv_;
    Flt flt = *fltMember;
    float cur = currentNote_;
    float g = filterG_;
    float gStep = filterGStep_;
    float effCutoff = effectiveCutoffHz_;
    bool snap = snapFilter_;
    float ph1 = wtPhase1_, ph2 = wtPhase2_, inc1 = wtInc1_, inc2 = wtInc2_;
    float pos = wtPos_, posStep = wtPosStep_;
    bool snapPos = snapWtPos_;
    int level = wtLevel_;
    int cs = controlSubsample_;

    const float target = targetNote_;
    const float glideAlpha = glideAlpha_;
    const float vel = velocity_;
    const float driftCents = driftCents_;
    const float drift2 = ParameterScaler::fastExp2(driftCents2_ * (1.0f / 1200.0f));
    const float detuneRatio = detuneRatio_;
    const float driftCutoff = driftCutoff_;
    const float baseCutoff = baseCutoffHz_;
    const float envAmount = filterEnvAmount_;
    const float keyTrack = keyTrack_;
    const float sr = sampleRate_;
    const float invSr = 1.0f / sampleRate_;
    const float drive = drive_;
    const int   table = timbre_.wtTable;
    const float basePos = timbre_.wtPosition;
    const float posEnv = timbre_.wtEnvAmount;
    constexpr float kMaxPos = static_cast<float>(Wavetables::FRAMES - 1);
    constexpr int   kS = Wavetables::STRIDE;
    const int16_t* levelBase = Wavetables::frame(table, level, 0);

    for (int i = 0; i < n; ++i) {
        glideStep(cur, target, glideAlpha);
        const float amp = aEnv.process() * vel;
        const float fEnv = fEnvGen.process();

        if (cs == 0) {
            const float freq = noteToHz(cur + bend[i] + driftCents * 0.01f);
            inc1 = std::min(freq * invSr, 0.45f);
            inc2 = std::min(inc1 * detuneRatio * drift2, 0.45f);
            level = Wavetables::levelForIncrement(std::max(inc1, inc2));
            levelBase = Wavetables::frame(table, level, 0);

            const float octaves = fEnv * envAmount * 4.0f + keyTrack * (cur - 60.0f) / 12.0f;
            effCutoff = std::clamp(baseCutoff * driftCutoff * ParameterScaler::fastExp2(octaves), 10.0f, sr * 0.45f);
            float gTarget = cutoffToGFast(effCutoff, sr);
            if constexpr (Ladder) gTarget = gTarget / (1.0f + gTarget);
            const float posTarget = std::clamp(basePos + posEnv * fEnv, 0.0f, 1.0f) * kMaxPos;
            if (snap) {
                g = gTarget;
                gStep = 0.0f;
                snap = false;
            } else {
                gStep = (gTarget - g) / static_cast<float>(CONTROL_INTERVAL);
            }
            if (snapPos) {
                pos = posTarget;
                posStep = 0.0f;
                snapPos = false;
            } else {
                posStep = (posTarget - pos) / static_cast<float>(CONTROL_INTERVAL);
            }
        }
        if (++cs >= CONTROL_INTERVAL) cs = 0;

        pos += posStep;
        const int   fi = std::clamp(static_cast<int>(pos), 0, Wavetables::FRAMES - 2);
        const float fr = pos - static_cast<float>(fi);
        const int16_t* fa = levelBase + fi * kS;
        const int16_t* fb = fa + kS;
        auto readFrame = [&](float ph) {
            const float x = ph * static_cast<float>(Wavetables::SIZE);
            const int   k = static_cast<int>(x);
            const float t = x - static_cast<float>(k);
            const float a0 = static_cast<float>(fa[k]);
            const float b0 = static_cast<float>(fb[k]);
            const float a = a0 + t * (static_cast<float>(fa[k + 1]) - a0);
            const float b = b0 + t * (static_cast<float>(fb[k + 1]) - b0);
            return a + fr * (b - a);
        };
        float osc = (readFrame(ph1) + readFrame(ph2)) * (0.5f * Wavetables::SAMPLE_SCALE);
        ph1 += inc1;
        if (ph1 >= 1.0f) ph1 -= 1.0f;
        ph2 += inc2;
        if (ph2 >= 1.0f) ph2 -= 1.0f;

        if constexpr (Drive) osc = ParameterScaler::softClip(osc * drive);

        g += gStep;
        float filtered;
        if constexpr (Ladder) {
            flt.setNormalizedGain(g);
            filtered = flt.process(osc);
        } else {
            flt.setG(g);
            filtered = flt.processLowpass(osc);
        }
        const float out = filtered * amp;
        acc[i] += out;

        if (!aEnv.isActive()) break;
    }

    ampEnv_ = aEnv;
    filterEnv_ = fEnvGen;
    *fltMember = flt;
    currentNote_ = cur;
    filterG_ = g;
    filterGStep_ = gStep;
    effectiveCutoffHz_ = effCutoff;
    snapFilter_ = snap;
    wtPhase1_ = ph1;
    wtPhase2_ = ph2;
    wtInc1_ = inc1;
    wtInc2_ = inc2;
    wtPos_ = pos;
    wtPosStep_ = posStep;
    snapWtPos_ = snapPos;
    wtLevel_ = level;
    controlSubsample_ = static_cast<uint8_t>(cs);
}

// -------------------------------------------------------------------------------------------------------
// Phase distortion (Casio CZ): one or two lines (the second detuned) -> DC blocker -> amp. The DCW amount
// is the base plus filterEnvAmount x filter envelope, scaled down as pitch rises (the CZ's DCW key follow),
// which also keeps the sharpened waveforms from aliasing.
// -------------------------------------------------------------------------------------------------------
template <PdWave W, bool Dual>
void Voice::renderPhaseDist(float* acc, int n, const float* bend) {
    Envelope aEnv = ampEnv_;
    Envelope fEnvGen = filterEnv_;
    float cur = currentNote_;
    float ph1 = pdPhase1_, ph2 = pdPhase2_, inc1 = pdInc1_, inc2 = pdInc2_;
    PdShape shape;
    shape.amount = pdAmount_; shape.knee = pdKnee_; shape.k1 = pdK1_; shape.k2 = pdK2_; shape.reso = pdResoRatio_;
    float dcX1 = dcX1_, dcY1 = dcY1_;
    int cs = controlSubsample_;

    const float target = targetNote_;
    const float glideAlpha = glideAlpha_;
    const float vel = velocity_;
    const float driftCents = driftCents_;
    const float drift2 = ParameterScaler::fastExp2(driftCents2_ * (1.0f / 1200.0f));
    const float detuneRatio = detuneRatio_;
    const float baseDcw = timbre_.pdDcw;
    const float envAmount = filterEnvAmount_;
    const float invSr = 1.0f / sampleRate_;
    const float dcCoef = dcCoef_;

    for (int i = 0; i < n; ++i) {
        glideStep(cur, target, glideAlpha);
        const float amp = aEnv.process() * vel;
        const float fEnv = fEnvGen.process();

        if (cs == 0) {
            const float freq = noteToHz(cur + bend[i] + driftCents * 0.01f);
            inc1 = std::min(freq * invSr, 0.45f);
            inc2 = Dual ? std::min(inc1 * detuneRatio * drift2, 0.45f) : inc1;
            const float incMax = std::max(inc1, inc2);
            const float dMax = std::clamp(1.0f - 10.0f * incMax, 0.0f, 0.98f);
            shape = pdShape(W, std::clamp(baseDcw + envAmount * fEnv, 0.0f, 1.0f) * dMax, incMax);
        }
        if (++cs >= CONTROL_INTERVAL) cs = 0;

        float x = pdSample<W>(ph1, shape);
        ph1 += inc1;
        if (ph1 >= 1.0f) ph1 -= 1.0f;
        if constexpr (Dual) {
            x = 0.5f * (x + pdSample<W>(ph2, shape));
            ph2 += inc2;
            if (ph2 >= 1.0f) ph2 -= 1.0f;
        }
        const float y = x - dcX1 + dcCoef * dcY1;
        dcX1 = x;
        dcY1 = y;

        const float out = y * amp;
        acc[i] += out;

        if (!aEnv.isActive()) break;
    }

    ampEnv_ = aEnv;
    filterEnv_ = fEnvGen;
    currentNote_ = cur;
    pdPhase1_ = ph1;
    pdPhase2_ = ph2;
    pdInc1_ = inc1;
    pdInc2_ = inc2;
    pdAmount_ = shape.amount;
    pdKnee_ = shape.knee;
    pdK1_ = shape.k1;
    pdK2_ = shape.k2;
    pdResoRatio_ = shape.reso;
    dcX1_ = dcX1;
    dcY1_ = dcY1;
    controlSubsample_ = static_cast<uint8_t>(cs);
}

template <bool Dual>
void Voice::renderPhaseDistWave(float* acc, int n, const float* bend) {
    switch (timbre_.pdWave) {
        case PdWave::Square:     renderPhaseDist<PdWave::Square, Dual>(acc, n, bend); break;
        case PdWave::Pulse:      renderPhaseDist<PdWave::Pulse, Dual>(acc, n, bend); break;
        case PdWave::DoubleSine: renderPhaseDist<PdWave::DoubleSine, Dual>(acc, n, bend); break;
        case PdWave::ResoSaw:    renderPhaseDist<PdWave::ResoSaw, Dual>(acc, n, bend); break;
        case PdWave::ResoTri:    renderPhaseDist<PdWave::ResoTri, Dual>(acc, n, bend); break;
        case PdWave::ResoTrap:   renderPhaseDist<PdWave::ResoTrap, Dual>(acc, n, bend); break;
        default:                 renderPhaseDist<PdWave::Saw, Dual>(acc, n, bend); break;
    }
}

// -------------------------------------------------------------------------------------------------------
// Pluck (Karplus-Strong): the line recirculates through a fractional-delay allpass and a one-zero lowpass
// with a loss per pass; the noise burst written at note-on is the pluck. Drive -> amp (the amp envelope
// is the player's damping: sustain holds the string, release mutes it).
// -------------------------------------------------------------------------------------------------------
void Voice::excitePluck(bool fromSilence, float velNorm) {
    if (pluckLine_.empty()) return;
    float* line = pluckLine_.data();

    pluckDamp_ = pluckDamping(timbre_.pluckBright);
    const float freq = noteToHz(currentNote_ + pitchBendSemitones_ + driftCents_ * 0.01f);
    const PluckLoop loop = pluckLoop(freq, sampleRate_, pluckDamp_, timbre_.pluckDecayMs);
    pluckDelay_ = loop.delay;
    pluckApCoef_ = loop.apCoef;
    pluckGain_ = loop.gain;

    if (fromSilence) {
        std::fill(pluckLine_.begin(), pluckLine_.end(), 0.0f);
        pluckApX1_ = pluckApY1_ = pluckLpPrev_ = 0.0f;
    } else {
        for (float& s : pluckLine_) s *= 0.3f; // re-plucking a ringing string damps it first
    }

    // Burst: one period of noise through a one-pole lowpass (brightness, harder picks brighter), mean
    // removed, then a comb at the pick position (the string's nodes there are not excited)
    const int len = std::max(2, loop.delay);
    const float bright = std::clamp(timbre_.pluckBright * (0.55f + 0.45f * velNorm), 0.02f, 1.0f);
    const float coef = 0.08f + 0.92f * bright * bright;
    const int start = pluckPos_ - len;
    float y = 0.0f, sum = 0.0f;
    // Scratch shared by all voices: note-ons run on one thread at a time (the audio thread in deferred
    // mode, the engine-locked caller otherwise), and 4 KB is too much for the audio task's stack
    static float burst[PLUCK_LINE_SIZE];
    for (int j = 0; j < len; ++j) {
        y += coef * (nextRandom() - y);
        burst[j] = y;
        sum += y;
    }
    const float mean = sum / static_cast<float>(len);
    for (int j = 0; j < len; ++j) burst[j] -= mean;
    const float pick = 0.03f + 0.47f * std::clamp(timbre_.pluckPick, 0.0f, 1.0f);
    const int m = std::max(1, static_cast<int>(pick * static_cast<float>(len) + 0.5f));
    for (int j = len - 1; j >= m; --j) burst[j] -= burst[j - m];
    float power = 0.0f;
    for (int j = 0; j < len; ++j) power += burst[j] * burst[j];
    const float gain = 0.8f / std::sqrt(std::max(power / static_cast<float>(len), 1.0e-9f)) * 0.5f;
    for (int j = 0; j < len; ++j) line[(start + j) & kPluckMask] += burst[j] * gain;
}

template <bool Drive>
void Voice::renderPluck(float* acc, int n, const float* bend) {
    if (pluckLine_.empty()) return;
    float* line = pluckLine_.data();

    Envelope aEnv = ampEnv_;
    float cur = currentNote_;
    int pos = pluckPos_;
    int delay = pluckDelay_;
    float apC = pluckApCoef_, apX1 = pluckApX1_, apY1 = pluckApY1_, lpPrev = pluckLpPrev_;
    float gain = pluckGain_;
    int cs = controlSubsample_;

    const float target = targetNote_;
    const float glideAlpha = glideAlpha_;
    const float vel = velocity_;
    const float driftCents = driftCents_;
    const float sr = sampleRate_;
    const float drive = drive_;
    const float damp = pluckDamping(timbre_.pluckBright);
    const float decayMs = timbre_.pluckDecayMs;

    for (int i = 0; i < n; ++i) {
        glideStep(cur, target, glideAlpha);
        const float amp = aEnv.process() * vel;

        if (cs == 0) {
            const PluckLoop loop = pluckLoop(noteToHz(cur + bend[i] + driftCents * 0.01f), sr, damp, decayMs);
            delay = loop.delay;
            apC = loop.apCoef;
            gain = loop.gain;
        }
        if (++cs >= CONTROL_INTERVAL) cs = 0;

        const float x = line[(pos - delay) & kPluckMask];
        const float a = apC * x + apX1 - apC * apY1;
        apX1 = x;
        apY1 = a;
        const float y = gain * ((1.0f - damp) * a + damp * lpPrev);
        lpPrev = a;
        line[pos] = y;
        pos = (pos + 1) & kPluckMask;

        float s = y;
        if constexpr (Drive) s = ParameterScaler::softClip(s * drive);
        const float out = s * amp;
        acc[i] += out;

        if (!aEnv.isActive()) break;
    }

    ampEnv_ = aEnv;
    currentNote_ = cur;
    pluckPos_ = pos;
    pluckDelay_ = delay;
    pluckApCoef_ = apC;
    pluckApX1_ = apX1;
    pluckApY1_ = apY1;
    pluckLpPrev_ = lpPrev;
    pluckGain_ = gain;
    pluckDamp_ = damp;
    controlSubsample_ = static_cast<uint8_t>(cs);
}

// -------------------------------------------------------------------------------------------------------
// Additive (Hammond tonewheel organ): harmonics 1..16 of the 16' pitch from one sine/cosine lookup and the
// Chebyshev recurrence sin(k x) = 2 cos(x) sin((k-1) x) - sin((k-2) x), weighted by the drawbars and
// dropped above Nyquist. Percussion adds the 4' or 2 2/3' harmonic under the filter envelope; key click is
// a few ms of differentiated noise.
// -------------------------------------------------------------------------------------------------------
void Voice::renderAdditive(float* acc, int n, const float* bend) {
    Envelope aEnv = ampEnv_;
    Envelope fEnvGen = filterEnv_;
    float cur = currentNote_;
    float phase = addPhase_;
    float inc = addInc_;
    float click = clickEnv_;
    float clickPrev = clickPrevNoise_;
    uint32_t rng = rng_;
    int cs = controlSubsample_;
    float w[ADDITIVE_HARMONICS + 1];
    std::copy(std::begin(addWeights_), std::end(addWeights_), w);
    bool percOn = addPercWeight_ > 0.0f && static_cast<float>(addPercHarmonic_) * inc < 0.45f;

    const float target = targetNote_;
    const float glideAlpha = glideAlpha_;
    const float vel = velocity_;
    const float driftCents = driftCents_;
    const float invSr = 1.0f / sampleRate_;
    const float clickDecay = clickDecay_;
    const int   percH = addPercHarmonic_;
    const float percWeight = addPercWeight_;

    for (int i = 0; i < n; ++i) {
        glideStep(cur, target, glideAlpha);
        const float amp = aEnv.process() * vel;
        const float fEnv = fEnvGen.process();

        if (cs == 0) {
            inc = 0.5f * noteToHz(cur + bend[i] + driftCents * 0.01f) * invSr; // 16' = one octave below
            for (int h = 1; h <= ADDITIVE_HARMONICS; ++h) {
                w[h] = static_cast<float>(h) * inc < 0.45f ? addBaseWeights_[h] : 0.0f;
            }
            percOn = percWeight > 0.0f && static_cast<float>(percH) * inc < 0.45f;
        }
        if (++cs >= CONTROL_INTERVAL) cs = 0;

        const float x = TWO_PI_F * phase;
        const float s1 = SineLut::lookupFast(x);
        const float twoC = 2.0f * SineLut::lookupFast(x + kHalfPi);
        const float wPerc = w[percH];
        if (percOn) w[percH] = wPerc + percWeight * fEnv;
        float out = w[1] * s1;
        float sPrev = 0.0f, sCur = s1;
        for (int h = 2; h <= ADDITIVE_HARMONICS; ++h) {
            const float sNext = twoC * sCur - sPrev;
            out += w[h] * sNext;
            sPrev = sCur;
            sCur = sNext;
        }
        w[percH] = wPerc;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;

        if (click > 1.0e-4f) {
            rng = rng * 1664525u + 1013904223u;
            const float noise = static_cast<float>(rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
            out += click * 0.5f * (noise - clickPrev);
            clickPrev = noise;
            click *= clickDecay;
        }

        const float o = out * amp;
        acc[i] += o;

        if (!aEnv.isActive()) break;
    }

    ampEnv_ = aEnv;
    filterEnv_ = fEnvGen;
    currentNote_ = cur;
    addPhase_ = phase;
    addInc_ = inc;
    clickEnv_ = click;
    clickPrevNoise_ = clickPrev;
    rng_ = rng;
    std::copy(w, w + ADDITIVE_HARMONICS + 1, std::begin(addWeights_));
    controlSubsample_ = static_cast<uint8_t>(cs);
}

template void Voice::renderWavetable<false, false>(float*, int, const float*);
template void Voice::renderWavetable<false, true>(float*, int, const float*);
template void Voice::renderWavetable<true, false>(float*, int, const float*);
template void Voice::renderWavetable<true, true>(float*, int, const float*);
template void Voice::renderPhaseDistWave<false>(float*, int, const float*);
template void Voice::renderPhaseDistWave<true>(float*, int, const float*);
template void Voice::renderPluck<false>(float*, int, const float*);
template void Voice::renderPluck<true>(float*, int, const float*);

Opx4Engine::Opx4Engine() {
    SineLut::init();
    updatePhaseIncs();
}

void Opx4Engine::init(float sampleRate) {
    SineLut::init();
    setSampleRate(sampleRate);
    reset();
}

void Opx4Engine::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        updatePhaseIncs();
    }
}

void Opx4Engine::reset() {
    for (auto& op : ops_) {
        op.phase = 0;
    }
    fbZ1_ = 0.0f;
    fbZ2_ = 0.0f;
}

void Opx4Engine::setAlgorithm(int algo) {
    algorithm_ = std::clamp(algo, 0, 7);
}

void Opx4Engine::setFrequency(float freqHz) {
    baseFreqHz_ = std::max(0.0f, freqHz);
    updatePhaseIncs();
}

void Opx4Engine::setOpRatio(int op, float ratio) {
    ops_[op & 3].ratio = std::max(0.0f, ratio);
    updatePhaseIncs();
}

void Opx4Engine::setOpLevel(int op, float level) {
    ops_[op & 3].level = std::max(0.0f, level);
}

void Opx4Engine::setOpDetuneHz(int op, float detuneHz) {
    ops_[op & 3].detuneHz = detuneHz;
    updatePhaseIncs();
}

void Opx4Engine::updatePhaseIncs() {
    for (int i = 0; i < 4; ++i) {
        float f = (baseFreqHz_ + ops_[i].detuneHz) * ops_[i].ratio;
        if (f < 0.0f) f = 0.0f;
        ops_[i].phaseInc = static_cast<uint32_t>((static_cast<double>(f) / static_cast<double>(sampleRate_)) * 4294967296.0);
    }
}

inline float Opx4Engine::computeOp(int opIdx, float modInRadians) {
    int32_t modPhase = static_cast<int32_t>(modInRadians * (4294967296.0f / TWO_PI_F));
    uint32_t effPhase = ops_[opIdx].phase + static_cast<uint32_t>(modPhase);
    float out = SineLut::lookupPhase32(effPhase) * ops_[opIdx].level;
    ops_[opIdx].phase += ops_[opIdx].phaseInc;
    return out;
}

float Opx4Engine::processSample() {
    float fb = (fbZ1_ + fbZ2_) * 0.5f * feedbackIndex_;
    float y4 = computeOp(3, fb);
    fbZ2_ = fbZ1_;
    fbZ1_ = y4;

    float out = 0.0f;
    switch (algorithm_) {
        case 0: {
            float y3 = computeOp(2, y4);
            float y2 = computeOp(1, y3);
            float y1 = computeOp(0, y2);
            out = y1;
            break;
        }
        case 1: {
            float y3 = computeOp(2, 0.0f);
            float y2 = computeOp(1, y4 + y3);
            float y1 = computeOp(0, y2);
            out = y1;
            break;
        }
        case 2: {
            float y3 = computeOp(2, y4);
            float y2 = computeOp(1, 0.0f);
            float y1 = computeOp(0, y3 + y2);
            out = y1;
            break;
        }
        case 3: {
            float y3 = computeOp(2, y4);
            float y2 = computeOp(1, y4);
            float y1 = computeOp(0, y3 + y2);
            out = y1;
            break;
        }
        case 4: {
            float y3 = computeOp(2, y4);
            float y2 = computeOp(1, 0.0f);
            float y1 = computeOp(0, y2);
            out = y1 + y3;
            break;
        }
        case 5: {
            float y3 = computeOp(2, y4);
            float y2 = computeOp(1, y4);
            float y1 = computeOp(0, y4);
            out = y1 + y2 + y3;
            break;
        }
        case 6: {
            float y3 = computeOp(2, y4);
            float y2 = computeOp(1, 0.0f);
            float y1 = computeOp(0, 0.0f);
            out = y1 + y2 + y3;
            break;
        }
        case 7: {
            float y3 = computeOp(2, 0.0f);
            float y2 = computeOp(1, 0.0f);
            float y1 = computeOp(0, 0.0f);
            out = y1 + y2 + y3 + y4;
            break;
        }
        default: {
            float y3 = computeOp(2, y4);
            float y2 = computeOp(1, y3);
            float y1 = computeOp(0, y2);
            out = y1;
            break;
        }
    }
    return out;
}

void Opx4Engine::renderBlock(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = processSample();
    }
}

void Opx4Engine::renderBlockAdd(float* acc, int n) {
    for (int i = 0; i < n; ++i) {
        acc[i] += processSample();
    }
}

} // namespace Synth
