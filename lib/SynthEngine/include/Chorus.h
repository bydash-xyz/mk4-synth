#pragma once

#include "Parameters.h"
#include "Filter.h"
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

enum class SolinaFormant : uint8_t {
    Bypass = 0,
    Horn   = 1,
    Violin = 2
};

class SolinaEnsemble {
public:
    SolinaEnsemble();

    void init(float sampleRate);
    void setSampleRate(float sampleRate);
    void reset();

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    void setMix(float wetMix) { wetMix_ = std::clamp(wetMix, 0.0f, 1.0f); }
    float getMix() const { return wetMix_; }

    void setFormantMode(SolinaFormant mode);
    SolinaFormant getFormantMode() const { return formantMode_; }

    void setFormantFrequencies(float f1, float f2, float f3 = 0.0f);

    void setSlowRateHz(float hz) { slowRateHz_ = std::clamp(hz, 0.05f, 5.0f); }
    float getSlowRateHz() const { return slowRateHz_; }

    void setFastRateHz(float hz) { fastRateHz_ = std::clamp(hz, 1.0f, 20.0f); }
    float getFastRateHz() const { return fastRateHz_; }

    void setCenterDelayMs(float ms) { centerDelayMs_ = std::clamp(ms, 1.0f, 20.0f); }
    float getCenterDelayMs() const { return centerDelayMs_; }

    void setSlowDepthMs(float ms) { slowDepthMs_ = std::clamp(ms, 0.1f, 10.0f); }
    float getSlowDepthMs() const { return slowDepthMs_; }

    void setFastDepthMs(float ms) { fastDepthMs_ = std::clamp(ms, 0.05f, 5.0f); }
    float getFastDepthMs() const { return fastDepthMs_; }

    void processSample(float inL, float inR, float& outL, float& outR);
    void processBlock(float* left, float* right, size_t numFrames);

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    bool enabled_{true};
    float wetMix_{1.0f};

    SolinaFormant formantMode_{SolinaFormant::Violin};
    float formantFreqs_[3]{320.0f, 2200.0f, 3200.0f};
    int activeFormants_{3};
    StateVariableFilter formantFilters_[3];

    static constexpr size_t BUFFER_SIZE = 2048;
    static constexpr size_t BUFFER_MASK = BUFFER_SIZE - 1;
    std::array<float, BUFFER_SIZE> buffer_{};
    size_t writeIdx_{0};

    float slowRateHz_{0.6f};
    float fastRateHz_{5.8f};
    float slowPhase_{0.0f};
    float fastPhase_{0.0f};

    float centerDelayMs_{6.0f};
    float slowDepthMs_{1.5f};
    float fastDepthMs_{0.3f};

    float readInterpolated(float delaySamples) const;
    float processFormants(float in);
    void updateFormantCoeffs();
};

inline void process_solina_ensemble(SolinaEnsemble& ensemble, float* left, float* right, size_t numFrames) {
    ensemble.processBlock(left, right, numFrames);
}

} // namespace Synth
