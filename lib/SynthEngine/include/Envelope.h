#pragma once

#include "Parameters.h"
#include <cstdint>

namespace Synth {

// Attack charges toward this level (above 1.0) so it reaches 1.0 with a punchy curve
static constexpr float kAttackTarget = 1.5f;

enum class EnvelopeState : uint8_t {
    Idle    = 0,
    Attack  = 1,
    Decay   = 2,
    Sustain = 3,
    Release = 4
};

class Envelope {
public:
    Envelope();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);

    // Times in milliseconds, sustain in [0.0f, 1.0f].
    // Attack time = time from 0 to peak (RC charge toward 1.5x overshoot, like analog ADSRs).
    // Decay / release time = time to cover 95% of the distance to the target (3 RC time constants).
    void setAttackMs(float attackMs);
    void setDecayMs(float decayMs);
    void setSustainLevel(float sustainLevel);
    void setReleaseMs(float releaseMs);

    void noteOn();
    void noteOff();
    // Release with a short fixed time regardless of the release setting (voice steal / mode change)
    void fastRelease(float timeMs = 8.0f);
    void reset();

    // Advance 1 sample and return current envelope amplitude [0.0f, 1.0f]
    float process();

    EnvelopeState getState() const { return state_; }
    bool isActive() const { return state_ != EnvelopeState::Idle; }
    float getCurrentValue() const { return currentLevel_; }

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};

    float attackMs_{10.0f};
    float decayMs_{200.0f};
    float sustainLevel_{0.7f};
    float releaseMs_{300.0f};

    // Exponential rate coefficients
    float attackCoef_{0.01f};
    float decayCoef_{0.001f};
    float releaseCoef_{0.001f};
    float fastReleaseCoef_{0.0f}; // > 0 while a fastRelease() is in progress

    float currentLevel_{0.0f};
    EnvelopeState state_{EnvelopeState::Idle};

    void updateRates();
    float timeToCoef(float timeMs) const;
    float attackTimeToCoef(float timeMs) const;
};

inline float Envelope::process() {
    switch (state_) {
        case EnvelopeState::Idle:
            currentLevel_ = 0.0f;
            break;

        case EnvelopeState::Attack:
            // Target slightly above 1.0 gives a punchy, linear-exponential attack curve
            currentLevel_ += (kAttackTarget - currentLevel_) * attackCoef_;
            if (currentLevel_ >= 1.0f) {
                currentLevel_ = 1.0f;
                state_ = EnvelopeState::Decay;
            }
            break;

        case EnvelopeState::Decay:
            currentLevel_ += (sustainLevel_ - currentLevel_) * decayCoef_;
            if (currentLevel_ <= sustainLevel_ + 0.0005f) {
                currentLevel_ = sustainLevel_;
                state_ = EnvelopeState::Sustain;
            }
            break;

        case EnvelopeState::Sustain:
            // Slew toward the sustain level so moving the knob during a held note doesn't step
            currentLevel_ += (sustainLevel_ - currentLevel_) * decayCoef_;
            break;

        case EnvelopeState::Release:
            // Target slightly below 0.0 ensures it reaches zero cleanly without a tail stall
            currentLevel_ += (-0.01f - currentLevel_) * (fastReleaseCoef_ > 0.0f ? fastReleaseCoef_ : releaseCoef_);
            if (currentLevel_ <= 0.0002f) {
                currentLevel_ = 0.0f;
                state_ = EnvelopeState::Idle;
                fastReleaseCoef_ = 0.0f;
            }
            break;
    }

    return currentLevel_;
}

} // namespace Synth
