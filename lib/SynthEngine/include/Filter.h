#pragma once

#include "Parameters.h"
#include <cmath>

namespace Synth {

enum class FilterType : uint8_t {
    Lowpass  = 0,
    Bandpass = 1,
    Highpass = 2,
    Notch    = 3
};

// Which analog filter topology a voice runs
enum class FilterModel : uint8_t {
    Svf12    = 0, // 12 dB/oct state-variable (Yamaha CS-80, Casio RC reconstruction)
    Ladder24 = 1  // 24 dB/oct 4-pole ladder (Moog, Roland IR3109, CEM3320, TB-303 approximation)
};

// Prewarped trapezoidal integrator gain for a cutoff frequency (clamped below Nyquist)
inline float cutoffToG(float cutoffHz, float sampleRate) {
    const float maxCutoff = sampleRate * 0.45f;
    const float fc = std::clamp(cutoffHz, 10.0f, maxCutoff);
    return std::tan(3.14159265358979323846f * fc / sampleRate);
}

// Same mapping using ParameterScaler::fastTan (<1e-6 relative error); for per-voice control-rate use
inline float cutoffToGFast(float cutoffHz, float sampleRate) {
    const float maxCutoff = sampleRate * 0.45f;
    const float fc = std::clamp(cutoffHz, 10.0f, maxCutoff);
    return ParameterScaler::fastTan(3.14159265358979323846f * fc / sampleRate);
}

class StateVariableFilter {
public:
    StateVariableFilter();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void setCutoff(float cutoffHz);
    // Set the integrator gain directly (per-sample cutoff ramps; see cutoffToG)
    void setG(float g);
    void setResonance(float q);
    void setType(FilterType type);
    void reset();

    // Process a single audio sample
    float process(float in);
    // process() for the Lowpass type, inlined for per-voice block renderers
    float processLowpass(float in);

    // Get current parameters

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float cutoffHz_{1000.0f};
    float resonanceQ_{0.707f};
    FilterType type_{FilterType::Lowpass};

    // Filter state variables (integrators)
    float s1_{0.0f};
    float s2_{0.0f};

    // Computed coefficients
    float g_{0.0f};
    float k_{1.0f / 0.707f}; // damping = 1/Q, only changes in setResonance
    float a1_{0.0f};
    float a2_{0.0f};
    float a3_{0.0f};

    void updateCoefficients();
};

inline void StateVariableFilter::setG(float g) {
    // Per-sample hot path: k_ is cached, so this is one reciprocal plus two multiplies
    g_ = g;
    if (g_ <= 0.0f) g_ = cutoffToG(cutoffHz_, sampleRate_);
    a1_ = ParameterScaler::fastRecip(1.0f + g_ * (g_ + k_));
    a2_ = g_ * a1_;
    a3_ = g_ * a2_;
}

// Cytomic SVF linear trapezoidal integration (see process()); returns the lowpass output
inline float StateVariableFilter::processLowpass(float in) {
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
    return v2;
}

// Zero-delay-feedback 4-pole transistor ladder (Zavalishin, "The Art of VA Filter Design", ch. 5)
// with a tanh stage on the feedback-summed input. Resonance steals passband gain as on a real
// ladder, and k >= 4 self-oscillates with amplitude bounded by the saturator.
class LadderFilter {
public:
    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void setCutoff(float cutoffHz);
    void setG(float g);
    // Set the already-normalised gain g / (1 + g) (per-sample ramps, no division)
    void setNormalizedGain(float G) { G_ = G; }
    // Resonance in the shared Q units (see ParameterScaler::resonanceQToLadderK)
    void setResonance(float q);
    void reset();

    float process(float in);

    float getFeedback() const { return k_; }

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    float G_{0.1f};     // g / (1 + g)
    float k_{0.0f};     // feedback 0..~4.2
    float makeup_{1.0f};
    float s_[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

inline float LadderFilter::process(float in) {
    const float G = G_;
    const float oneMinusG = 1.0f - G;
    const float G2 = G * G;
    const float G3 = G2 * G;
    const float G4 = G2 * G2;

    // Each TPT one-pole stage is y = G*x + (1 - G)*s, so stage i responds to the ladder input u as
    // y_i = G^i * u + c_i, with the c_i depending only on the states. Computing the c_i first gives
    // the feedback term (sigma = c_4) and lets all four outputs follow from u in parallel, instead
    // of through a 12-operation serial chain after the saturator.
    const float c1 = oneMinusG * s_[0];
    const float c2 = G * c1 + oneMinusG * s_[1];
    const float c3 = G * c2 + oneMinusG * s_[2];
    const float sigma = G * c3 + oneMinusG * s_[3];

    // Solve the zero-delay feedback loop linearly, then saturate the ladder input
    // (headroom of 2: ~8% compression at unity, so oscillator levels drive it warmly, not fuzzily)
    float u = (in - k_ * sigma) * ParameterScaler::fastRecip(1.0f + k_ * G4);
    u = ParameterScaler::fastTanhScaled(u, 2.0f);

    // Stage outputs, and the trapezoidal state update s <- 2y - s
    const float y1 = G * u + c1;
    const float y2 = G2 * u + c2;
    const float y3 = G3 * u + c3;
    const float y4 = G4 * u + sigma;
    s_[0] = 2.0f * y1 - s_[0];
    s_[1] = 2.0f * y2 - s_[1];
    s_[2] = 2.0f * y3 - s_[2];
    s_[3] = 2.0f * y4 - s_[3];

    // One denormal guard for the whole cascade (the four states decay together)
    if (std::abs(s_[3]) < 1e-15f) s_[0] = s_[1] = s_[2] = s_[3] = 0.0f;
    return y4 * makeup_;
}

} // namespace Synth
