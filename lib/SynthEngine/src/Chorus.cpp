#include "Chorus.h"
#include "SineLut.h"
#include <algorithm>
#include <cmath>

namespace Synth {

StereoChorus::StereoChorus() {
    SineLut::init();
}

void StereoChorus::init(float sampleRate) {
    SineLut::init();
    setSampleRate(sampleRate);
    reset();
}

void StereoChorus::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        bbdLpfCoef_ = 1.0f - std::exp(-2.0f * 3.14159265f * 9000.0f / sampleRate_);
    }
}

void StereoChorus::setRateHz(float rateHz) {
    rateHz_ = std::clamp(rateHz, 0.1f, 10.0f);
}

void StereoChorus::setDepthMs(float depthMs) {
    depthMs_ = std::clamp(depthMs, 0.1f, 5.0f);
}

void StereoChorus::setMix(float wetMix) {
    wetMix_ = std::clamp(wetMix, 0.0f, 1.0f);
}

void StereoChorus::reset() {
    bufferL_.fill(0.0f);
    bufferR_.fill(0.0f);
    writeIdx_ = 0;
    lfoPhase_ = 0.0f;
    bbdLpfL_ = 0.0f;
    bbdLpfR_ = 0.0f;
}

float StereoChorus::readInterpolated(const std::array<float, BUFFER_SIZE>& buf, float delaySamples) const {
    float clampedDelay = std::clamp(delaySamples, 1.0f, static_cast<float>(BUFFER_SIZE - 2));
    float readPos = static_cast<float>(writeIdx_) - clampedDelay;
    if (readPos < 0.0f) readPos += static_cast<float>(BUFFER_SIZE);

    size_t idx0 = static_cast<size_t>(readPos) & BUFFER_MASK;
    size_t idx1 = (idx0 + 1) & BUFFER_MASK;
    float frac = readPos - static_cast<float>(static_cast<size_t>(readPos));

    return buf[idx0] + frac * (buf[idx1] - buf[idx0]);
}

inline void StereoChorus::processFrame(float& l, float& r, float dtLfo, float centerSamples,
                                       float depthSamples, float dryGain, float wetGain) {
    const float inL = l;
    const float inR = r;

    bufferL_[writeIdx_] = inL;
    bufferR_[writeIdx_] = inR;

    // Juno-60 style: one triangle LFO drives the two BBD lines in antiphase (L = +tri, R = -tri)
    const float tri = (lfoPhase_ < 0.5f) ? (4.0f * lfoPhase_ - 1.0f) : (3.0f - 4.0f * lfoPhase_);
    lfoPhase_ += dtLfo;
    if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;

    float chorusL = readInterpolated(bufferL_, centerSamples + depthSamples * tri);
    float chorusR = readInterpolated(bufferR_, centerSamples - depthSamples * tri);

    bbdLpfL_ += (chorusL - bbdLpfL_) * bbdLpfCoef_;
    bbdLpfR_ += (chorusR - bbdLpfR_) * bbdLpfCoef_;

    l = inL * dryGain + bbdLpfL_ * wetGain;
    r = inR * dryGain + bbdLpfR_ * wetGain;

    writeIdx_ = (writeIdx_ + 1) & BUFFER_MASK;
}

// Dry/wet gains keep the wet:dry balance of the mix knob but are power-normalised, so turning the
// chorus up changes the texture, not the loudness of broadband material
static inline void chorusGains(float mix, float& dryGain, float& wetGain) {
    const float dry = 1.0f - mix * 0.5f;
    const float wet = mix;
    const float norm = 1.0f / std::sqrt(dry * dry + wet * wet);
    dryGain = dry * norm;
    wetGain = wet * norm;
}

void StereoChorus::processSample(float inL, float inR, float& outL, float& outR) {
    outL = inL;
    outR = inR;
    if (!enabled_ || wetMix_ <= 0.001f) {
        return;
    }
    float dryGain, wetGain;
    chorusGains(wetMix_, dryGain, wetGain);
    processFrame(outL, outR, rateHz_ / sampleRate_, (centerDelayMs_ * 0.001f) * sampleRate_,
                 (depthMs_ * 0.001f) * sampleRate_, dryGain, wetGain);
}

void StereoChorus::processBlock(float* left, float* right, size_t numFrames) {
    if (!enabled_ || wetMix_ <= 0.001f) {
        return;
    }
    const float dtLfo = rateHz_ / sampleRate_;
    const float centerSamples = (centerDelayMs_ * 0.001f) * sampleRate_;
    const float depthSamples  = (depthMs_ * 0.001f) * sampleRate_;
    float dryGain, wetGain;
    chorusGains(wetMix_, dryGain, wetGain);

    for (size_t i = 0; i < numFrames; ++i) {
        processFrame(left[i], right[i], dtLfo, centerSamples, depthSamples, dryGain, wetGain);
    }
}

SolinaEnsemble::SolinaEnsemble() {
    SineLut::init();
    setFormantMode(SolinaFormant::Violin);
}

void SolinaEnsemble::init(float sampleRate) {
    SineLut::init();
    setSampleRate(sampleRate);
    reset();
}

void SolinaEnsemble::setSampleRate(float sampleRate) {
    if (sampleRate > 0.0f) {
        sampleRate_ = sampleRate;
        updateFormantCoeffs();
    }
}

void SolinaEnsemble::reset() {
    buffer_.fill(0.0f);
    writeIdx_ = 0;
    slowPhase_ = 0.0f;
    fastPhase_ = 0.0f;
    for (int i = 0; i < 3; ++i) {
        formantFilters_[i].reset();
    }
}

void SolinaEnsemble::setFormantMode(SolinaFormant mode) {
    formantMode_ = mode;
    switch (mode) {
        case SolinaFormant::Horn:
            formantFreqs_[0] = 550.0f;
            formantFreqs_[1] = 1400.0f;
            formantFreqs_[2] = 0.0f;
            activeFormants_ = 2;
            break;
        case SolinaFormant::Violin:
            formantFreqs_[0] = 320.0f;
            formantFreqs_[1] = 2200.0f;
            formantFreqs_[2] = 3200.0f;
            activeFormants_ = 3;
            break;
        case SolinaFormant::Bypass:
        default:
            activeFormants_ = 0;
            break;
    }
    updateFormantCoeffs();
}

void SolinaEnsemble::setFormantFrequencies(float f1, float f2, float f3) {
    formantFreqs_[0] = f1;
    formantFreqs_[1] = f2;
    formantFreqs_[2] = f3;
    activeFormants_ = (f3 > 10.0f) ? 3 : ((f2 > 10.0f) ? 2 : ((f1 > 10.0f) ? 1 : 0));
    updateFormantCoeffs();
}

void SolinaEnsemble::updateFormantCoeffs() {
    for (int i = 0; i < 3; ++i) {
        formantFilters_[i].setType(FilterType::Bandpass);
        formantFilters_[i].setSampleRate(sampleRate_);
        formantFilters_[i].setResonance(3.0f);
        if (i < activeFormants_ && formantFreqs_[i] > 10.0f) {
            formantFilters_[i].setCutoff(formantFreqs_[i]);
        }
    }
}

float SolinaEnsemble::processFormants(float in) {
    if (formantMode_ == SolinaFormant::Bypass || activeFormants_ == 0) {
        return in;
    }
    float sum = 0.0f;
    for (int i = 0; i < activeFormants_; ++i) {
        sum += formantFilters_[i].process(in);
    }
    return sum;
}

float SolinaEnsemble::readInterpolated(float delaySamples) const {
    float clampedDelay = std::clamp(delaySamples, 1.0f, static_cast<float>(BUFFER_SIZE - 2));
    float readPos = static_cast<float>(writeIdx_) - clampedDelay;
    if (readPos < 0.0f) readPos += static_cast<float>(BUFFER_SIZE);

    size_t idx0 = static_cast<size_t>(readPos) & BUFFER_MASK;
    size_t idx1 = (idx0 + 1) & BUFFER_MASK;
    float frac = readPos - static_cast<float>(static_cast<size_t>(readPos));

    return buffer_[idx0] + frac * (buffer_[idx1] - buffer_[idx0]);
}

void SolinaEnsemble::processSample(float inL, float inR, float& outL, float& outR) {
    if (!enabled_) {
        outL = inL;
        outR = inR;
        return;
    }

    float monoIn = 0.5f * (inL + inR);
    float filteredIn = processFormants(monoIn);

    buffer_[writeIdx_] = filteredIn;

    const float dtSlow = slowRateHz_ / sampleRate_;
    const float dtFast = fastRateHz_ / sampleRate_;

    const float centerSamples = centerDelayMs_ * 0.001f * sampleRate_;
    const float slowDepthSamples = slowDepthMs_ * 0.001f * sampleRate_;
    const float fastDepthSamples = fastDepthMs_ * 0.001f * sampleRate_;

    constexpr float phi[3] = {0.0f, 1.0f / 3.0f, 2.0f / 3.0f};
    float taps[3];

    for (int k = 0; k < 3; ++k) {
        float pSlow = slowPhase_ + phi[k];
        if (pSlow >= 1.0f) pSlow -= 1.0f;
        float pFast = fastPhase_ + phi[k];
        if (pFast >= 1.0f) pFast -= 1.0f;

        float sSlow = SineLut::lookupFast(pSlow * TWO_PI_F);
        float sFast = SineLut::lookupFast(pFast * TWO_PI_F);

        float delaySamples = centerSamples + slowDepthSamples * sSlow + fastDepthSamples * sFast;
        taps[k] = readInterpolated(delaySamples);
    }

    slowPhase_ += dtSlow;
    if (slowPhase_ >= 1.0f) slowPhase_ -= 1.0f;
    fastPhase_ += dtFast;
    if (fastPhase_ >= 1.0f) fastPhase_ -= 1.0f;

    writeIdx_ = (writeIdx_ + 1) & BUFFER_MASK;

    const float tap1 = taps[0];
    const float tap2 = taps[1];
    const float tap3 = taps[2];

    const float wetL = 0.577f * (tap1 + 0.5f * tap2 - 0.5f * tap3);
    const float wetR = 0.577f * (tap2 + 0.5f * tap3 - 0.5f * tap1);

    outL = inL * (1.0f - wetMix_) + wetL * wetMix_;
    outR = inR * (1.0f - wetMix_) + wetR * wetMix_;
}

void SolinaEnsemble::processBlock(float* left, float* right, size_t numFrames) {
    for (size_t i = 0; i < numFrames; ++i) {
        processSample(left[i], right[i], left[i], right[i]);
    }
}

} // namespace Synth
