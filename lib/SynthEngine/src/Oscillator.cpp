#include "Oscillator.h"
#include <algorithm>

namespace Synth {

Oscillator::Oscillator() {
    updatePhaseInc();
}

void Oscillator::init(float sampleRate) {
    setSampleRate(sampleRate);
    resetPhase();
}

void Oscillator::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        updatePhaseInc();
    }
}

void Oscillator::setWaveform(Waveform wave) {
    waveform_ = wave;
}

void Oscillator::setPulseWidth(float pw) {
    pulseWidth_ = std::clamp(pw, 0.05f, 0.95f);
}

void Oscillator::resetPhase(float phase) {
    phase_ = phase;
    while (phase_ >= 1.0f) phase_ -= 1.0f;
    while (phase_ < 0.0f) phase_ += 1.0f;
    // Start the integrator on the ideal triangle for this phase (-1 at 0, +1 at 0.5) so the
    // first cycle has no DC offset and never hits the clamp
    triIntegrator_ = (phase_ < 0.5f) ? (-1.0f + 4.0f * phase_) : (3.0f - 4.0f * phase_);
}

} // namespace Synth
