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

StagePianoEngine::StagePianoEngine() {
    reset();
}

void StagePianoEngine::init(float sampleRate) {
    sampleRate_ = sampleRate;
    reset();
}

void StagePianoEngine::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        float freqHz = 440.0f * ParameterScaler::fastExp2((static_cast<float>(note_) - 69.0f) * (1.0f / 12.0f));
        updateTuning(freqHz, velocityNorm_);
    }
}

void StagePianoEngine::reset() {
    delayLine_.fill(0.0f);
    writePos_ = 0;
    thiranX1_ = 0.0f;
    thiranY1_ = 0.0f;
    dispX1_ = 0.0f;
    dispY1_ = 0.0f;
    dampPrev_ = 0.0f;
    active_ = false;
    releaseGain_ = 1.0f;
    hammerPos_ = 0;
    hammerLen_ = 0;
    updateTuning(440.0f, 1.0f);
}

void StagePianoEngine::updateTuning(float freqHz, float velNorm) {
    if (freqHz <= 0.0f) freqHz = 440.0f;
    float T0 = sampleRate_ / freqHz;
    float w0 = TWO_PI_F * freqHz / sampleRate_;

    dispC_ = -0.015f;
    float phiDisp = -w0 + 2.0f * std::atan2(dispC_ * std::sin(w0), 1.0f + dispC_ * std::cos(w0));
    float dDisp = -phiDisp / w0;

    dampD_ = 0.35f - 0.20f * std::clamp(velNorm, 0.0f, 1.0f);
    float reDamp = (1.0f - dampD_) + dampD_ * std::cos(w0);
    float imDamp = -dampD_ * std::sin(w0);
    float phiDamp = std::atan2(imDamp, reDamp);
    float dDamp = -phiDamp / w0;

    float dRem = T0 - dDisp - dDamp;
    delayM_ = static_cast<int>(std::floor(dRem - 0.5f));
    if (delayM_ < 2) delayM_ = 2;
    if (delayM_ > static_cast<int>(DELAY_BUFFER_SIZE - 4)) delayM_ = static_cast<int>(DELAY_BUFFER_SIZE - 4);
    float dTh = dRem - static_cast<float>(delayM_);

    float denom = std::sin(w0 * (1.0f + dTh) * 0.5f);
    if (std::abs(denom) > 1e-9f) {
        thiranA_ = std::sin(w0 * (1.0f - dTh) * 0.5f) / denom;
    } else {
        thiranA_ = (1.0f - dTh) / (1.0f + dTh);
    }
}

void StagePianoEngine::noteOn(uint8_t noteNumber, uint8_t velocity) {
    note_ = noteNumber;
    velocityNorm_ = std::clamp(static_cast<float>(velocity) / 127.0f, 0.01f, 1.0f);
    float freqHz = 440.0f * ParameterScaler::fastExp2((static_cast<float>(noteNumber) - 69.0f) * (1.0f / 12.0f));
    updateTuning(freqHz, velocityNorm_);

    delayLine_.fill(0.0f);
    writePos_ = 0;
    thiranX1_ = 0.0f;
    thiranY1_ = 0.0f;
    dispX1_ = 0.0f;
    dispY1_ = 0.0f;
    dampPrev_ = 0.0f;
    releaseGain_ = 1.0f;

    int tauSamples = std::clamp(static_cast<int>(sampleRate_ * (0.0025f - 0.0015f * velocityNorm_)), 4, 120);
    hammerLen_ = static_cast<size_t>(tauSamples);
    hammerPos_ = 0;
    float amp = velocityNorm_ * 0.8f;
    for (size_t i = 0; i < hammerLen_; ++i) {
        hammerBuffer_[i] = 0.5f * (1.0f - std::cos(TWO_PI_F * static_cast<float>(i) / static_cast<float>(hammerLen_))) * amp;
    }
    active_ = true;
}

void StagePianoEngine::noteOff() {
    releaseGain_ = 0.998f;
    releaseCoef_ = 0.994f;
}

float StagePianoEngine::processSample() {
    if (!active_) return 0.0f;

    size_t rdPos = (writePos_ + DELAY_BUFFER_SIZE - static_cast<size_t>(delayM_)) % DELAY_BUFFER_SIZE;
    float x = delayLine_[rdPos];

    float thY = thiranA_ * x + thiranX1_ - thiranA_ * thiranY1_;
    thiranX1_ = x;
    thiranY1_ = thY;

    float dispY = dispC_ * thY + dispX1_ - dispC_ * dispY1_;
    dispX1_ = thY;
    dispY1_ = dispY;

    float dampY = (1.0f - dampD_) * dispY + dampD_ * dampPrev_;
    dampPrev_ = dispY;

    float excite = 0.0f;
    if (hammerPos_ < hammerLen_) {
        excite = hammerBuffer_[hammerPos_++];
    }

    float inp = excite + loopGain_ * dampY;
    delayLine_[writePos_] = inp;
    writePos_ = (writePos_ + 1) % DELAY_BUFFER_SIZE;

    float out = dispY * releaseGain_;
    if (releaseGain_ < 0.999f) {
        releaseGain_ *= releaseCoef_;
        if (releaseGain_ < 1e-4f) active_ = false;
    }
    return out;
}

void StagePianoEngine::renderBlock(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = processSample();
    }
}

void StagePianoEngine::renderBlockAdd(float* acc, int n) {
    for (int i = 0; i < n; ++i) {
        acc[i] += processSample();
    }
}

StageEpEngine::StageEpEngine() {
    SineLut::init();
    init(sampleRate_);
}

void StageEpEngine::init(float sampleRate) {
    sampleRate_ = sampleRate;
    ampEnv_.init(sampleRate_);
    ampEnv_.setAttackMs(1.0f);
    ampEnv_.setDecayMs(1800.0f);
    ampEnv_.setSustainLevel(0.4f);
    ampEnv_.setReleaseMs(200.0f);
    reset();
}

void StageEpEngine::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        ampEnv_.setSampleRate(sampleRate_);
        carrierInc_ = freqHz_ / sampleRate_;
        modInc_ = (freqHz_ * 14.0f) / sampleRate_;
    }
}

void StageEpEngine::reset() {
    carrierPhase_ = 0.0f;
    modPhase_ = 0.0f;
    modEnv_ = 0.0f;
    phaserPhase_ = 0.0f;
    tremoloPhase_ = 0.0f;
    apX1_.fill(0.0f);
    apY1_.fill(0.0f);
    ampEnv_.reset();
}

void StageEpEngine::noteOn(uint8_t noteNumber, uint8_t velocity) {
    float velNorm = std::clamp(static_cast<float>(velocity) / 127.0f, 0.01f, 1.0f);
    freqHz_ = 440.0f * ParameterScaler::fastExp2((static_cast<float>(noteNumber) - 69.0f) * (1.0f / 12.0f));
    carrierInc_ = freqHz_ / sampleRate_;
    modInc_ = (freqHz_ * 14.0f) / sampleRate_;
    carrierPhase_ = 0.0f;
    modPhase_ = 0.0f;

    modEnv_ = 1.0f;
    modDecayCoef_ = std::exp(-1.0f / (sampleRate_ * 0.04f));
    fmIndex_ = 0.5f + 3.0f * velNorm;
    drive_ = 1.0f + 4.5f * (velNorm * velNorm);

    ampEnv_.noteOn();
}

void StageEpEngine::noteOff() {
    ampEnv_.noteOff();
}

void StageEpEngine::processStereo(float& outL, float& outR) {
    if (!ampEnv_.isActive()) {
        outL = 0.0f;
        outR = 0.0f;
        return;
    }

    float mod = SineLut::lookupFast(TWO_PI_F * modPhase_) * fmIndex_ * modEnv_;
    modEnv_ *= modDecayCoef_;

    float car = SineLut::lookupFast(TWO_PI_F * carrierPhase_ + mod);

    carrierPhase_ += carrierInc_;
    if (carrierPhase_ >= 1.0f) carrierPhase_ -= 1.0f;
    modPhase_ += modInc_;
    if (modPhase_ >= 1.0f) modPhase_ -= 1.0f;

    float u = car * drive_;
    float w = std::tanh(u + 0.35f * u * u);

    float lfo = SineLut::lookupFast(TWO_PI_F * phaserPhase_);
    phaserPhase_ += phaserRateHz_ / sampleRate_;
    if (phaserPhase_ >= 1.0f) phaserPhase_ -= 1.0f;

    float apCoeff = 0.4f + 0.3f * lfo;
    float sig = w;
    for (int stage = 0; stage < 4; ++stage) {
        float apOut = apCoeff * sig + apX1_[stage] - apCoeff * apY1_[stage];
        apX1_[stage] = sig;
        apY1_[stage] = apOut;
        sig = apOut;
    }
    float phased = 0.65f * w + 0.35f * sig;

    float tremLfo = SineLut::lookupFast(TWO_PI_F * tremoloPhase_);
    tremoloPhase_ += tremoloRateHz_ / sampleRate_;
    if (tremoloPhase_ >= 1.0f) tremoloPhase_ -= 1.0f;

    float amp = ampEnv_.process();
    outL = phased * (1.0f + tremoloDepth_ * tremLfo) * 0.5f * amp;
    outR = phased * (1.0f - tremoloDepth_ * tremLfo) * 0.5f * amp;
}

float StageEpEngine::processSample() {
    float outL = 0.0f, outR = 0.0f;
    processStereo(outL, outR);
    return 0.5f * (outL + outR);
}

void StageEpEngine::renderBlock(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = processSample();
    }
}

void StageEpEngine::renderBlockStereo(float* left, float* right, int n) {
    for (int i = 0; i < n; ++i) {
        processStereo(left[i], right[i]);
    }
}

void StageEpEngine::renderBlockAdd(float* acc, int n) {
    for (int i = 0; i < n; ++i) {
        acc[i] += processSample();
    }
}

StudioStringsEngine::StudioStringsEngine() {
    init(sampleRate_);
}

void StudioStringsEngine::init(float sampleRate) {
    sampleRate_ = sampleRate;
    ampEnv_.init(sampleRate_);
    ampEnv_.setAttackMs(80.0f);
    ampEnv_.setDecayMs(400.0f);
    ampEnv_.setSustainLevel(0.85f);
    ampEnv_.setReleaseMs(300.0f);

    formant1_.init(sampleRate_);
    formant1_.setType(FilterType::Bandpass);
    formant1_.setCutoff(280.0f);
    formant1_.setResonance(3.5f);

    formant2_.init(sampleRate_);
    formant2_.setType(FilterType::Bandpass);
    formant2_.setCutoff(450.0f);
    formant2_.setResonance(4.0f);

    formant3_.init(sampleRate_);
    formant3_.setType(FilterType::Bandpass);
    formant3_.setCutoff(2500.0f);
    formant3_.setResonance(3.0f);

    reset();
}

void StudioStringsEngine::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        ampEnv_.setSampleRate(sampleRate_);
        formant1_.setSampleRate(sampleRate_);
        formant2_.setSampleRate(sampleRate_);
        formant3_.setSampleRate(sampleRate_);
        updateIncs();
    }
}

void StudioStringsEngine::reset() {
    phase1_ = 0.0f;
    phase2_ = 0.3f;
    noisePrev_ = 0.0f;
    ampEnv_.reset();
    formant1_.reset();
    formant2_.reset();
    formant3_.reset();
    updateIncs();
}

void StudioStringsEngine::setDetuneCents(float cents) {
    detuneCents_ = cents;
    updateIncs();
}

void StudioStringsEngine::updateIncs() {
    inc1_ = freqHz_ / sampleRate_;
    inc2_ = (freqHz_ * ParameterScaler::fastExp2(detuneCents_ * (1.0f / 1200.0f))) / sampleRate_;
}

void StudioStringsEngine::noteOn(uint8_t noteNumber, uint8_t velocity) {
    velNorm_ = std::clamp(static_cast<float>(velocity) / 127.0f, 0.01f, 1.0f);
    freqHz_ = 440.0f * ParameterScaler::fastExp2((static_cast<float>(noteNumber) - 69.0f) * (1.0f / 12.0f));
    updateIncs();
    phase1_ = 0.0f;
    phase2_ = 0.3f;
    ampEnv_.noteOn();
}

void StudioStringsEngine::noteOff() {
    ampEnv_.noteOff();
}

float StudioStringsEngine::processSample() {
    if (!ampEnv_.isActive()) return 0.0f;

    float dt1 = inc1_;
    float s1 = 2.0f * phase1_ - 1.0f;
    if (phase1_ < dt1) {
        float t = phase1_ / dt1;
        s1 -= (2.0f * t - t * t - 1.0f);
    } else if (phase1_ > 1.0f - dt1) {
        float t = (phase1_ - 1.0f) / dt1;
        s1 -= (2.0f * t + t * t + 1.0f);
    }
    phase1_ += dt1;
    if (phase1_ >= 1.0f) phase1_ -= 1.0f;

    float dt2 = inc2_;
    float s2 = 2.0f * phase2_ - 1.0f;
    if (phase2_ < dt2) {
        float t = phase2_ / dt2;
        s2 -= (2.0f * t - t * t - 1.0f);
    } else if (phase2_ > 1.0f - dt2) {
        float t = (phase2_ - 1.0f) / dt2;
        s2 -= (2.0f * t + t * t + 1.0f);
    }
    phase2_ += dt2;
    if (phase2_ >= 1.0f) phase2_ -= 1.0f;

    rng_ = rng_ * 1664525u + 1013904223u;
    float white = static_cast<float>(rng_ >> 8) * (2.0f / 16777216.0f) - 1.0f;
    float noise = white - 0.7f * noisePrev_;
    noisePrev_ = white;
    float friction = noise * 0.06f * (0.5f + 0.5f * velNorm_);

    float in = 0.5f * (s1 + s2) + friction;

    float f1 = formant1_.process(in);
    float f2 = formant2_.process(in);
    float f3 = formant3_.process(in);
    float body = 0.8f * f1 + 0.7f * f2 + 0.5f * f3;

    float amp = ampEnv_.process() * velNorm_;
    return (0.35f * in + 0.65f * body) * amp;
}

void StudioStringsEngine::renderBlock(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = processSample();
    }
}

void StudioStringsEngine::renderBlockAdd(float* acc, int n) {
    for (int i = 0; i < n; ++i) {
        acc[i] += processSample();
    }
}

FabricXlEngine::FabricXlEngine() {
    SineLut::init();
    init(sampleRate_);
}

void FabricXlEngine::init(float sampleRate) {
    sampleRate_ = sampleRate;
    ampEnv_.init(sampleRate_);
    ampEnv_.setAttackMs(15.0f);
    ampEnv_.setDecayMs(350.0f);
    ampEnv_.setSustainLevel(0.7f);
    ampEnv_.setReleaseMs(350.0f);
    reset();
}

void FabricXlEngine::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        ampEnv_.setSampleRate(sampleRate_);
        updateIncs();
    }
}

void FabricXlEngine::reset() {
    wtPhase1_ = 0.0f;
    wtPhase2_ = 0.25f;
    chimePhase_ = 0.0f;
    chimeEnv_ = 0.0f;
    breathEnv_ = 0.0f;
    ampEnv_.reset();
    updateIncs();
}

void FabricXlEngine::setDetuneCents(float cents) {
    detuneCents_ = cents;
    updateIncs();
}

void FabricXlEngine::updateIncs() {
    inc1_ = freqHz_ / sampleRate_;
    inc2_ = (freqHz_ * ParameterScaler::fastExp2(detuneCents_ * (1.0f / 1200.0f))) / sampleRate_;
    wtLevel1_ = Wavetables::isReady() ? Wavetables::levelForIncrement(inc1_) : 0;
    wtLevel2_ = Wavetables::isReady() ? Wavetables::levelForIncrement(inc2_) : 0;
}

void FabricXlEngine::noteOn(uint8_t noteNumber, uint8_t velocity) {
    float velNorm = std::clamp(static_cast<float>(velocity) / 127.0f, 0.01f, 1.0f);
    freqHz_ = 440.0f * ParameterScaler::fastExp2((static_cast<float>(noteNumber) - 69.0f) * (1.0f / 12.0f));
    updateIncs();
    wtPhase1_ = 0.0f;
    wtPhase2_ = 0.25f;

    chimePhase_ = 0.0f;
    chimeInc_ = std::min((freqHz_ * 4.0f) / sampleRate_, 0.45f);
    chimeEnv_ = velNorm * 0.4f;
    chimeDecay_ = std::exp(-1.0f / (sampleRate_ * 0.045f));

    breathEnv_ = velNorm * 0.25f;
    breathDecay_ = std::exp(-1.0f / (sampleRate_ * 0.030f));

    ampEnv_.noteOn();
}

void FabricXlEngine::noteOff() {
    ampEnv_.noteOff();
}

float FabricXlEngine::processSample() {
    if (!ampEnv_.isActive()) return 0.0f;

    auto readWt = [&](float phase, int table, int level) {
        if (!Wavetables::isReady()) {
            return SineLut::lookupFast(TWO_PI_F * phase);
        }
        constexpr int kS = Wavetables::STRIDE;
        const int16_t* fa = Wavetables::frame(table, level, Wavetables::FRAMES / 2);
        float x = phase * static_cast<float>(Wavetables::SIZE);
        int k = static_cast<int>(x);
        float t = x - static_cast<float>(k);
        float a0 = static_cast<float>(fa[k]);
        float a1 = static_cast<float>(fa[k + 1]);
        return (a0 + t * (a1 - a0)) * Wavetables::SAMPLE_SCALE;
    };

    float osc1 = readWt(wtPhase1_, 0, wtLevel1_);
    float osc2 = readWt(wtPhase2_, 2, wtLevel2_);

    wtPhase1_ += inc1_;
    if (wtPhase1_ >= 1.0f) wtPhase1_ -= 1.0f;
    wtPhase2_ += inc2_;
    if (wtPhase2_ >= 1.0f) wtPhase2_ -= 1.0f;

    float chime = SineLut::lookupFast(TWO_PI_F * chimePhase_) * chimeEnv_;
    chimePhase_ += chimeInc_;
    if (chimePhase_ >= 1.0f) chimePhase_ -= 1.0f;
    chimeEnv_ *= chimeDecay_;

    rng_ = rng_ * 1664525u + 1013904223u;
    float white = static_cast<float>(rng_ >> 8) * (2.0f / 16777216.0f) - 1.0f;
    float breath = white * breathEnv_;
    breathEnv_ *= breathDecay_;

    float out = (0.5f * (osc1 + osc2) + chime + breath) * ampEnv_.process();
    return out;
}

void FabricXlEngine::renderBlock(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = processSample();
    }
}

void FabricXlEngine::renderBlockAdd(float* acc, int n) {
    for (int i = 0; i < n; ++i) {
        acc[i] += processSample();
    }
}

MarianaEngine::MarianaEngine() {
    SineLut::init();
    init(sampleRate_);
}

void MarianaEngine::init(float sampleRate) {
    sampleRate_ = sampleRate;
    ampEnv_.init(sampleRate_);
    ampEnv_.setAttackMs(5.0f);
    ampEnv_.setDecayMs(400.0f);
    ampEnv_.setSustainLevel(0.8f);
    ampEnv_.setReleaseMs(150.0f);

    midHpf_.init(sampleRate_);
    midHpf_.setType(FilterType::Highpass);
    midHpf_.setCutoff(120.0f);
    midHpf_.setResonance(0.707f);

    reset();
}

void MarianaEngine::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        ampEnv_.setSampleRate(sampleRate_);
        midHpf_.setSampleRate(sampleRate_);
        inc_ = freqHz_ / sampleRate_;
    }
}

void MarianaEngine::reset() {
    phase_ = 0.0f;
    dcZ1_ = 0.0f;
    dcIn1_ = 0.0f;
    ampEnv_.reset();
    midHpf_.reset();
    inc_ = freqHz_ / sampleRate_;
}

void MarianaEngine::setFrequency(float freqHz) {
    if (freqHz > 0.0f) {
        freqHz_ = freqHz;
        inc_ = freqHz_ / sampleRate_;
    }
}

void MarianaEngine::noteOn(uint8_t noteNumber, uint8_t velocity) {
    (void)velocity;
    freqHz_ = 440.0f * ParameterScaler::fastExp2((static_cast<float>(noteNumber) - 69.0f) * (1.0f / 12.0f));
    inc_ = freqHz_ / sampleRate_;
    phase_ = 0.0f;
    midHpf_.reset();
    dcZ1_ = 0.0f;
    dcIn1_ = 0.0f;
    ampEnv_.noteOn();
}

void MarianaEngine::noteOff() {
    ampEnv_.noteOff();
}

float MarianaEngine::processSample() {
    float p = phase_;
    phase_ += inc_;
    if (phase_ >= 1.0f) phase_ -= 1.0f;

    float subPhase = p - 0.21f;
    if (subPhase < 0.0f) subPhase += 1.0f;
    float sub = 0.8f * SineLut::lookupFast(TWO_PI_F * subPhase);

    float dt = inc_;
    float saw = 2.0f * p - 1.0f;
    if (p < dt) {
        float t = p / dt;
        saw -= (2.0f * t - t * t - 1.0f);
    } else if (p > 1.0f - dt) {
        float t = (p - 1.0f) / dt;
        saw -= (2.0f * t + t * t + 1.0f);
    }

    float hp = midHpf_.process(saw);
    float mid = tube_saturate_sample(hp, drive_, 0.1f, &dcZ1_, &dcIn1_);

    float amp = ampEnv_.process();
    return (sub + mid) * amp;
}

void MarianaEngine::renderPaths(float* subOut, float* midOut, int n) {
    for (int i = 0; i < n; ++i) {
        float p = phase_;
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;

        float subPhase = p - 0.21f;
        if (subPhase < 0.0f) subPhase += 1.0f;
        float sub = 0.8f * SineLut::lookupFast(TWO_PI_F * subPhase);

        float dt = inc_;
        float saw = 2.0f * p - 1.0f;
        if (p < dt) {
            float t = p / dt;
            saw -= (2.0f * t - t * t - 1.0f);
        } else if (p > 1.0f - dt) {
            float t = (p - 1.0f) / dt;
            saw -= (2.0f * t + t * t + 1.0f);
        }

        float hp = midHpf_.process(saw);
        float mid = tube_saturate_sample(hp, drive_, 0.1f, &dcZ1_, &dcIn1_);

        if (subOut) subOut[i] = sub;
        if (midOut) midOut[i] = mid;
    }
}

void MarianaEngine::renderBlock(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = processSample();
    }
}

void MarianaEngine::renderBlockAdd(float* acc, int n) {
    for (int i = 0; i < n; ++i) {
        acc[i] += processSample();
    }
}

HypeEngine::HypeEngine() {
    SineLut::init();
    init(sampleRate_);
}

void HypeEngine::init(float sampleRate) {
    sampleRate_ = sampleRate;
    ampEnv_.init(sampleRate_);
    ampEnv_.setAttackMs(5.0f);
    ampEnv_.setDecayMs(250.0f);
    ampEnv_.setSustainLevel(0.75f);
    ampEnv_.setReleaseMs(200.0f);
    reset();
}

void HypeEngine::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        ampEnv_.setSampleRate(sampleRate_);
        updateIncs();
    }
}

void HypeEngine::reset() {
    stereoBuf_.fill(0.0f);
    stereoWritePos_ = 0;
    lfoPhase_ = 0.0f;
    phase1_ = 0.0f;
    phase2_ = 0.0f;
    ampEnv_.reset();
    updateIncs();
}

void HypeEngine::setDetuneCents(float cents) {
    detuneCents_ = cents;
    updateIncs();
}

void HypeEngine::updateIncs() {
    float halfDetune = detuneCents_ * 0.5f;
    inc1_ = (freqHz_ * ParameterScaler::fastExp2(-halfDetune * (1.0f / 1200.0f))) / sampleRate_;
    inc2_ = (freqHz_ * ParameterScaler::fastExp2(halfDetune * (1.0f / 1200.0f))) / sampleRate_;
}

void HypeEngine::noteOn(uint8_t noteNumber, uint8_t velocity) {
    (void)velocity;
    freqHz_ = 440.0f * ParameterScaler::fastExp2((static_cast<float>(noteNumber) - 69.0f) * (1.0f / 12.0f));
    updateIncs();

    rng_ = rng_ * 1664525u + 1013904223u;
    phase1_ = static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
    rng_ = rng_ * 1664525u + 1013904223u;
    phase2_ = static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);

    ampEnv_.noteOn();
}

void HypeEngine::noteOff() {
    ampEnv_.noteOff();
}

void HypeEngine::processStereo(float& outL, float& outR) {
    if (!ampEnv_.isActive()) {
        outL = 0.0f;
        outR = 0.0f;
        return;
    }

    float dt1 = inc1_;
    float s1 = 2.0f * phase1_ - 1.0f;
    if (phase1_ < dt1) {
        float t = phase1_ / dt1;
        s1 -= (2.0f * t - t * t - 1.0f);
    } else if (phase1_ > 1.0f - dt1) {
        float t = (phase1_ - 1.0f) / dt1;
        s1 -= (2.0f * t + t * t + 1.0f);
    }
    phase1_ += dt1;
    if (phase1_ >= 1.0f) phase1_ -= 1.0f;

    float dt2 = inc2_;
    float s2 = 2.0f * phase2_ - 1.0f;
    if (phase2_ < dt2) {
        float t = phase2_ / dt2;
        s2 -= (2.0f * t - t * t - 1.0f);
    } else if (phase2_ > 1.0f - dt2) {
        float t = (phase2_ - 1.0f) / dt2;
        s2 -= (2.0f * t + t * t + 1.0f);
    }
    phase2_ += dt2;
    if (phase2_ >= 1.0f) phase2_ -= 1.0f;

    float monoMix = 0.5f * (s1 + s2);

    stereoBuf_[stereoWritePos_] = monoMix;
    float lfo = SineLut::lookupFast(TWO_PI_F * lfoPhase_);
    lfoPhase_ += 0.4f / sampleRate_;
    if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;

    int delayL = static_cast<int>(sampleRate_ * (0.007f + 0.002f * lfo));
    int delayR = static_cast<int>(sampleRate_ * (0.011f - 0.002f * lfo));

    size_t rdL = (stereoWritePos_ + STEREO_BUFFER_SIZE - static_cast<size_t>(delayL)) % STEREO_BUFFER_SIZE;
    size_t rdR = (stereoWritePos_ + STEREO_BUFFER_SIZE - static_cast<size_t>(delayR)) % STEREO_BUFFER_SIZE;
    stereoWritePos_ = (stereoWritePos_ + 1) % STEREO_BUFFER_SIZE;

    float delL = stereoBuf_[rdL];
    float delR = stereoBuf_[rdR];

    float amp = ampEnv_.process();
    outL = (monoMix + stereoSpread_ * delL) * 0.6f * amp;
    outR = (monoMix - stereoSpread_ * delR) * 0.6f * amp;
}

float HypeEngine::processSample() {
    float l = 0.0f, r = 0.0f;
    processStereo(l, r);
    return 0.5f * (l + r);
}

void HypeEngine::renderBlock(float* out, int n) {
    for (int i = 0; i < n; ++i) {
        out[i] = processSample();
    }
}

void HypeEngine::renderBlockStereo(float* left, float* right, int n) {
    for (int i = 0; i < n; ++i) {
        processStereo(left[i], right[i]);
    }
}

void HypeEngine::renderBlockAdd(float* acc, int n) {
    for (int i = 0; i < n; ++i) {
        acc[i] += processSample();
    }
}

void apply_hype_pump(float* buf_l, float* buf_r, int frames, float pump_depth, float* pump_phase, float pump_inc) {
    if (!pump_phase) return;
    float phase = *pump_phase;
    for (int i = 0; i < frames; ++i) {
        float inv_p = 1.0f - phase;
        float duck_gain = 1.0f - (pump_depth * (inv_p * inv_p * inv_p));
        if (buf_l) buf_l[i] *= duck_gain;
        if (buf_r) buf_r[i] *= duck_gain;
        phase += pump_inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    *pump_phase = phase;
}

} // namespace Synth

