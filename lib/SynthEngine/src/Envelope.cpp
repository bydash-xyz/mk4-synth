#include "Envelope.h"
#include <algorithm>
#include <cmath>

namespace Synth {

Envelope::Envelope() {
    updateRates();
}

void Envelope::init(float sampleRate) {
    setSampleRate(sampleRate);
    reset();
}

void Envelope::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        updateRates();
    }
}

void Envelope::setAttackMs(float attackMs) {
    attackMs_ = std::max(0.5f, attackMs);
    attackCoef_ = attackTimeToCoef(attackMs_);
}

void Envelope::setDecayMs(float decayMs) {
    decayMs_ = std::max(1.0f, decayMs);
    decayCoef_ = timeToCoef(decayMs_);
}

void Envelope::setSustainLevel(float sustainLevel) {
    sustainLevel_ = std::clamp(sustainLevel, 0.0f, 1.0f);
}

void Envelope::setReleaseMs(float releaseMs) {
    releaseMs_ = std::max(1.0f, releaseMs);
    releaseCoef_ = timeToCoef(releaseMs_);
}

void Envelope::updateRates() {
    attackCoef_ = attackTimeToCoef(attackMs_);
    decayCoef_ = timeToCoef(decayMs_);
    releaseCoef_ = timeToCoef(releaseMs_);
}

float Envelope::timeToCoef(float timeMs) const {
    // One-pole approach that covers 95% of the distance (3 time constants) in timeMs
    float samples = (timeMs * 0.001f) * sampleRate_;
    if (samples < 1.0f) samples = 1.0f;
    return 1.0f - std::exp(-3.0f / samples);
}

float Envelope::attackTimeToCoef(float timeMs) const {
    // Charging from 0 toward kAttackTarget, reach 1.0 in exactly timeMs:
    // (target - 1) / target = (1 - c)^n  ->  c = 1 - exp(ln((target - 1) / target) / n)
    float samples = (timeMs * 0.001f) * sampleRate_;
    if (samples < 1.0f) samples = 1.0f;
    return 1.0f - std::exp(std::log((kAttackTarget - 1.0f) / kAttackTarget) / samples);
}

void Envelope::noteOn() {
    state_ = EnvelopeState::Attack;
    fastReleaseCoef_ = 0.0f;
    // We do not reset currentLevel_ to 0 here; starting from currentLevel_ avoids clicks
    // during fast re-triggering of notes.
}

void Envelope::noteOff() {
    if (state_ != EnvelopeState::Idle) {
        state_ = EnvelopeState::Release;
    }
}

void Envelope::fastRelease(float timeMs) {
    if (state_ == EnvelopeState::Idle) return;
    state_ = EnvelopeState::Release;
    fastReleaseCoef_ = timeToCoef(timeMs);
}

void Envelope::reset() {
    state_ = EnvelopeState::Idle;
    fastReleaseCoef_ = 0.0f;
    currentLevel_ = 0.0f;
}

} // namespace Synth
