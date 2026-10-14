#include "Filter.h"
#include <algorithm>

namespace Synth {

StateVariableFilter::StateVariableFilter() {
    updateCoefficients();
}

void StateVariableFilter::init(float sampleRate) {
    setSampleRate(sampleRate);
    reset();
}

void StateVariableFilter::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        setCutoff(cutoffHz_);
    }
}

void StateVariableFilter::setCutoff(float cutoffHz) {
    cutoffHz_ = std::clamp(cutoffHz, 10.0f, sampleRate_ * 0.45f);
    g_ = cutoffToG(cutoffHz_, sampleRate_);
    updateCoefficients();
}

void StateVariableFilter::setResonance(float q) {
    // Q from 0.5 (critically damped) to 25.0
    resonanceQ_ = std::clamp(q, 0.5f, 25.0f);
    k_ = 1.0f / resonanceQ_;
    updateCoefficients();
}

void StateVariableFilter::setType(FilterType type) {
    type_ = type;
}

void StateVariableFilter::reset() {
    s1_ = 0.0f;
    s2_ = 0.0f;
}

void StateVariableFilter::updateCoefficients() {
    if (g_ <= 0.0f) g_ = cutoffToG(cutoffHz_, sampleRate_);
    a1_ = ParameterScaler::fastRecip(1.0f + g_ * (g_ + k_));
    a2_ = g_ * a1_;
    a3_ = g_ * a2_;
}

float StateVariableFilter::process(float in) {
    // Cytomic SVF linear trapezoidal integration
    float v3 = in - s2_;
    float v1 = a1_ * s1_ + a2_ * v3;
    float v2 = s2_ + a2_ * s1_ + a3_ * v3;

    // Continuous saturation of the integrator states: ~2% at the |s| ~ 2 that unity-level input
    // produces, bounded at 8.0 for stability at extreme Q, and smooth everywhere (no threshold remap).
    s1_ = ParameterScaler::fastTanhScaled(2.0f * v1 - s1_, 8.0f);
    s2_ = ParameterScaler::fastTanhScaled(2.0f * v2 - s2_, 8.0f);

    // Flush subnormals to zero
    if (std::abs(s1_) < 1e-15f) s1_ = 0.0f;
    if (std::abs(s2_) < 1e-15f) s2_ = 0.0f;

    switch (type_) {
        case FilterType::Lowpass:
            return v2;
        case FilterType::Bandpass:
            return v1;
        case FilterType::Highpass:
            return in - k_ * v1 - v2;
        case FilterType::Notch:
            return in - k_ * v1;
        default:
            return v2;
    }
}

// ---------------------------------------------------------------------------
// LadderFilter
// ---------------------------------------------------------------------------

void LadderFilter::init(float sampleRate) {
    setSampleRate(sampleRate);
    reset();
}

void LadderFilter::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) sampleRate_ = sampleRate;
}

void LadderFilter::setCutoff(float cutoffHz) {
    setG(cutoffToG(cutoffHz, sampleRate_));
}

void LadderFilter::setG(float g) {
    G_ = g / (1.0f + g);
}

void LadderFilter::setResonance(float q) {
    k_ = ParameterScaler::resonanceQToLadderK(q);
    // Partial passband make-up: a real ladder loses ~1/(1+k) of its bass at high resonance.
    // Restoring half of it keeps the characteristic thinning without large preset level jumps.
    makeup_ = 1.0f + 0.5f * k_;
}

void LadderFilter::reset() {
    s_[0] = s_[1] = s_[2] = s_[3] = 0.0f;
}

} // namespace Synth
