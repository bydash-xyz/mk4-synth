#pragma once

#include "Parameters.h"
#include <array>
#include <cmath>

namespace Synth {

class StereoChorus {
public:
    StereoChorus();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    void setRateHz(float rateHz);
    float getRateHz() const { return rateHz_; }

    void setDepthMs(float depthMs);
    float getDepthMs() const { return depthMs_; }

    void setMix(float wetMix); // 0.0 to 1.0
    float getMix() const { return wetMix_; }

    void reset();

    // Single-sample stereo processing (useful for sample-by-sample DAC ISRs)
    void processSample(float inL, float inR, float& outL, float& outR);

    // In-place stereo block processing
    void processBlock(float* left, float* right, size_t numFrames);

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    bool  enabled_{false};
    float rateHz_{0.8f};
    float depthMs_{2.5f};
    float centerDelayMs_{6.0f};
    float wetMix_{0.5f};

    // Delay buffer (power of 2 for fast bitmask wrapping)
    static constexpr size_t BUFFER_SIZE = 2048;
    static constexpr size_t BUFFER_MASK = BUFFER_SIZE - 1;

    // Static storage: a heap vector > 4 KB would be placed in PSRAM on ESP32-S3 (SPIRAM_MALLOC_ALWAYSINTERNAL=4096)
    std::array<float, BUFFER_SIZE> bufferL_{};
    std::array<float, BUFFER_SIZE> bufferR_{};
    size_t writeIdx_{0};

    // LFO phase
    float lfoPhase_{0.0f};

    // BBD output smoothing (the MN3009 lines are followed by ~9 kHz low-pass filters)
    float bbdLpfCoef_{0.7f};
    float bbdLpfL_{0.0f};
    float bbdLpfR_{0.0f};

    float readInterpolated(const std::array<float, BUFFER_SIZE>& buf, float delaySamples) const;
    inline void processFrame(float& l, float& r, float dtLfo, float centerSamples, float depthSamples,
                             float dryGain, float wetGain);
};

} // namespace Synth
