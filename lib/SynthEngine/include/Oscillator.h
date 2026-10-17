#pragma once

#include "Parameters.h"
#include <algorithm>
#include <cmath>

namespace Synth {

class Oscillator {
public:
    Oscillator();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void setFrequency(float freqHz);
    void setWaveform(Waveform wave);
    void setPulseWidth(float pw); // 0.05 to 0.95
    void resetPhase(float phase = 0.0f);
    // Move the phase without touching waveform state (used to phase-lock the sub-oscillator)
    void setPhaseOnly(float phase) { phase_ = phase; }
    float getPhase() const { return phase_; }
    // True if the last process() call wrapped the phase accumulator
    bool didWrap() const { return wrapped_; }

    Waveform getWaveform() const { return waveform_; }

    // Compute next audio sample [-1.0f, 1.0f]
    float process();
    // Same as process() with the waveform fixed at compile time (block renderers hoist the switch).
    // Waveform::Count renders silence but still advances the phase, like process() does.
    template <Waveform W> float processWave();

private:
    // Polynomial Band-Limited Step (PolyBLEP) residual
    static inline float polyBLEP(float t, float dt) {
        if (dt <= 0.0f) return 0.0f;
        if (t < dt) {
            float r = t / dt;
            return r + r - r * r - 1.0f; // 2*r - r^2 - 1
        } else if (t > 1.0f - dt) {
            float r = (t - 1.0f) / dt;
            return r * r + r + r + 1.0f; // r^2 + 2*r + 1
        }
        return 0.0f;
    }

    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float frequency_{440.0f};
    float phaseInc_{0.01f};
    float phase_{0.0f};
    float pulseWidth_{0.5f};
    bool  wrapped_{false};
    Waveform waveform_{Waveform::Sawtooth};

    // Leaky integrator state for integrated triangle
    float triIntegrator_{0.0f};

    void updatePhaseInc();
};

inline void Oscillator::updatePhaseInc() {
    phaseInc_ = frequency_ / sampleRate_;
    // Keep phase increment below Nyquist limit
    phaseInc_ = std::clamp(phaseInc_, 0.000001f, 0.499f);
}

inline void Oscillator::setFrequency(float freqHz) {
    frequency_ = std::max(0.1f, freqHz);
    updatePhaseInc();
}

template <Waveform W>
inline float Oscillator::processWave() {
    float out = 0.0f;
    const float dt = phaseInc_;

    if constexpr (W == Waveform::Sawtooth) {
        // Naive Sawtooth [-1.0, 1.0]
        float rawSaw = 2.0f * phase_ - 1.0f;
        // Subtract PolyBLEP at the transition (phase_ = 0)
        out = rawSaw - polyBLEP(phase_, dt);
    } else if constexpr (W == Waveform::Square) {
        // Naive Pulse/Square [-1.0, 1.0]
        float rawSquare = (phase_ < pulseWidth_) ? 1.0f : -1.0f;
        // Transition 1: rising edge at phase = 0
        rawSquare += polyBLEP(phase_, dt);
        // Transition 2: falling edge at phase = pulseWidth
        float phase2 = phase_ - pulseWidth_;
        if (phase2 < 0.0f) phase2 += 1.0f;
        rawSquare -= polyBLEP(phase2, dt);
        out = rawSquare;
    } else if constexpr (W == Waveform::Triangle) {
        // Triangle generated via leaky integrator of PolyBLEP square wave
        // producing a smooth, band-limited triangle without aliasing
        float rawSquare = (phase_ < 0.5f) ? 1.0f : -1.0f;
        rawSquare += polyBLEP(phase_, dt);
        float phase2 = phase_ - 0.5f;
        if (phase2 < 0.0f) phase2 += 1.0f;
        rawSquare -= polyBLEP(phase2, dt);

        // Leaky integrator: dy = 4 * dt * square
        const float leak = 1.0f - (dt * 0.5f);
        triIntegrator_ = triIntegrator_ * leak + (4.0f * dt) * rawSquare;
        // Clamp within bounds to prevent any DC drift
        triIntegrator_ = std::clamp(triIntegrator_, -1.2f, 1.2f);
        out = triIntegrator_;
    }

    // Advance phase accumulator
    phase_ += dt;
    wrapped_ = phase_ >= 1.0f;
    if (wrapped_) {
        phase_ -= 1.0f;
    }

    return out;
}

inline float Oscillator::process() {
    switch (waveform_) {
        case Waveform::Sawtooth: return processWave<Waveform::Sawtooth>();
        case Waveform::Square:   return processWave<Waveform::Square>();
        case Waveform::Triangle: return processWave<Waveform::Triangle>();
        default:                 return processWave<Waveform::Count>(); // silent, phase still advances
    }
}

inline float polyBlepResidual(float t) {
    if (t < 0.0f || t >= 1.0f) return 0.0f;
    return t + t - t * t - 1.0f;
}

inline float process_hard_sync_saw(float* master_phase, float master_inc, float* slave_phase, float slave_inc) {
    *master_phase += master_inc;
    *slave_phase += slave_inc;

    float out = (2.0f * (*slave_phase)) - 1.0f;

    if (*slave_phase >= 1.0f) {
        *slave_phase -= 1.0f;
        out = (2.0f * (*slave_phase)) - 1.0f;
        out -= polyBlepResidual(*slave_phase / slave_inc);
    }

    if (*master_phase >= 1.0f) {
        *master_phase -= 1.0f;
        float frac_master = *master_phase / master_inc;
        float slave_at_reset = *slave_phase - (frac_master * slave_inc);
        if (slave_at_reset < 0.0f) slave_at_reset += 1.0f;
        float discontinuity_mag = slave_at_reset;
        *slave_phase = frac_master * slave_inc;
        out = (2.0f * (*slave_phase)) - 1.0f;
        out -= discontinuity_mag * polyBlepResidual(frac_master);
    }

    return out;
}

inline float ring_modulate(float osc1_out, float osc2_out, float* dc_z1, float* dc_in1, float R = 0.995f) {
    float prod = osc1_out * osc2_out;
    float dc_out = prod - *dc_in1 + (R * (*dc_z1));
    *dc_in1 = prod;
    *dc_z1 = dc_out;
    return dc_out;
}

inline float cross_modulate(float osc1_inc, float osc2_out, float xmod_depth) {
    float warped = osc1_inc + (osc2_out * xmod_depth * osc1_inc);
    return std::clamp(warped, 0.0f, 0.48f);
}

} // namespace Synth

