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

} // namespace Synth
