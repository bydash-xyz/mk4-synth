#pragma once
// On-demand profiling for the ESP32-S3 firmware. Compiled in only when SYNTH_PROFILE is defined
// (build the `esp32s3_rev2_profile` PlatformIO environment); in the normal firmware every macro
// below expands to nothing, so there is no code, data or runtime cost.
//
// Output goes to the USB serial port once per second (~344 audio blocks):
//   [BENCH] ...  one-off engine/primitive benchmark measured at boot (repeated with each report)
//   [PROFILE] audio ...  render time of one 128-frame block: avg/max in us, blocks over the budget
//   [PROFILE] oled [anim] ...   OLED render time and frame period (20 frames per report)
// Capture it with tools/serial_capture.py.

#if defined(SYNTH_PROFILE) && defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <esp_timer.h>
#include <cstdio>
#include "SynthEngine.h"

namespace Profile {

constexpr int64_t AUDIO_BUDGET_US = 2902; // 128 frames at 44.1 kHz
constexpr uint32_t AUDIO_REPORT_BLOCKS = 344;
constexpr uint32_t OLED_REPORT_FRAMES = 20;

inline char g_bench[1400];

// Renders each configuration for 100 blocks on a private engine and records the mean us/block
// (single core, no voice worker), then times the DSP primitives in isolation.
inline void runBootBench() {
    using namespace Synth;
    static SynthEngine engine;
    static float left[128], right[128];
    int n = 0;
    auto add = [&](const char* fmt, const char* tag, long long v) {
        n += snprintf(g_bench + n, sizeof(g_bench) - n, fmt, tag, v);
    };

    n += snprintf(g_bench + n, sizeof(g_bench) - n, "[BENCH] us/block single-core:");
    auto engineCfg = [&](const char* tag, int preset, int voices) {
        engine.init(44100.0f);
        engine.setDeferredEvents(false);
        engine.setVoiceWorker(nullptr);
        engine.loadPresetIndex(preset);
        engine.setChorusEnabled(false);
        const int notes[] = {60, 62, 64, 67, 69, 71, 72, 74};
        for (int i = 0; i < voices && i < 8; i++) engine.noteOn(notes[i], 100);
        for (int i = 0; i < 20; i++) engine.processBlockFloat(left, right, 128);
        const int64_t t = esp_timer_get_time();
        for (int i = 0; i < 100; i++) engine.processBlockFloat(left, right, 128);
        add(" %s=%lld", tag, (esp_timer_get_time() - t) / 100);
        engine.panic();
    };
    engineCfg("juno0v", 0, 0);
    engineCfg("juno1v", 0, 1);
    engineCfg("juno5v", 0, 5);
    engineCfg("juno6v", 0, 6);
    engineCfg("obxa8v", 5, 8);
    engineCfg("cs805v", 4, 5);
    engineCfg("fm5v", 10, 5);

    n += snprintf(g_bench + n, sizeof(g_bench) - n, "\n[BENCH] ns/sample:");
    volatile float sink = 0;
    auto prim = [&](const char* tag, auto fn) {
        const int64_t t = esp_timer_get_time();
        float acc = 0;
        for (int i = 0; i < 12800; i++) acc += fn(i);
        sink = acc;
        add(" %s=%lld", tag, (esp_timer_get_time() - t) * 1000 / 12800);
    };
    { Oscillator o; o.init(44100.0f); o.setWaveform(Waveform::Sawtooth); o.setFrequency(261.6f);
      prim("saw", [&](int) { return o.process(); }); }
    { LadderFilter f; f.init(44100.0f); f.setCutoff(1400.0f); f.setResonance(2.5f);
      prim("ladder", [&](int i) { return f.process(((i & 63) - 32) * 0.03f); }); }
    { StateVariableFilter f; f.init(44100.0f); f.setCutoff(1400.0f); f.setResonance(2.5f);
      prim("svf", [&](int i) { f.setG(0.1f + (i & 1) * 1e-4f); return f.processLowpass(((i & 63) - 32) * 0.03f); }); }
    { Envelope e; e.init(44100.0f); e.setAttackMs(10); e.setDecayMs(300); e.setSustainLevel(0.6f); e.noteOn();
      prim("env", [&](int) { return e.process(); }); }
    { Voice v; v.init(44100.0f); v.noteOn(60, 100, 1);
      prim("voice", [&](int) { return v.process(); }); }  // 1-sample renderBlock wrapper
    { // The engine's path: whole 128-frame blocks per voice (compare with "voice" above)
      Voice v; v.init(44100.0f); v.noteOn(60, 100, 1);
      static float blk[128], bend[128];
      const int64_t t = esp_timer_get_time();
      for (int b = 0; b < 100; b++) v.renderBlock(blk, 128, bend);
      add(" %s=%lld", "voiceblk", (esp_timer_get_time() - t) * 1000 / 12800);
      sink = blk[0]; }
    { float x = 1;
      prim("fdiv", [&](int i) { x = 1.0f / (1.0f + (i & 31) * 0.1f); return x; }); }
    { float d = 1.0f;
      prim("frecip", [&](int i) { return ParameterScaler::fastRecip(d + (i & 31) * 0.1f); }); }
    { // Worst relative error of fastRecip against the ROM division, in parts per billion
      double worst = 0;
      for (int i = 0; i < 20000; i++) {
          const float d = 0.5f * ParameterScaler::fastExp2(i * (20.0f / 20000.0f));
          const double e = std::fabs((double)ParameterScaler::fastRecip(d) * d - 1.0);
          if (e > worst) worst = e;
      }
      add(" %s=%lld", "rcperr_ppb", (long long)(worst * 1e9)); }
    (void)sink;
    n += snprintf(g_bench + n, sizeof(g_bench) - n, "\n");
}

// Called with the time one audio block took to render.
inline void audioBlock(int64_t us, const Synth::SynthEngine* engine) {
    static int64_t sum = 0, max = 0;
    static uint32_t count = 0, over = 0;
    if (us > max) max = us;
    if (us > AUDIO_BUDGET_US) over++;
    sum += us;
    if (++count >= AUDIO_REPORT_BLOCKS) {
        Serial.print(g_bench);
        Serial.printf("[PROFILE] audio avg=%lldus max=%lldus over_budget=%u/%u active_voices=%u\n",
                      sum / AUDIO_REPORT_BLOCKS, max, over, AUDIO_REPORT_BLOCKS,
                      engine ? static_cast<unsigned>(engine->getActiveVoiceCount()) : 0u);
        sum = max = 0;
        count = over = 0;
    }
}

// Called after each OLED frame with the frame's start time and the active animation's name.
inline void oledFrame(int64_t startUs, const char* animName) {
    static int64_t sumRender = 0, maxRender = 0, sumPeriod = 0, maxPeriod = 0, lastEnd = 0;
    static uint32_t frames = 0;
    const int64_t end = esp_timer_get_time();
    const int64_t render = end - startUs;
    if (lastEnd) {
        const int64_t period = end - lastEnd;
        sumPeriod += period;
        if (period > maxPeriod) maxPeriod = period;
    }
    lastEnd = end;
    sumRender += render;
    if (render > maxRender) maxRender = render;
    if (++frames >= OLED_REPORT_FRAMES) {
        Serial.printf("[PROFILE] oled [%s] (loop on core %d) render avg=%lldms max=%lldms | frame period avg=%lldms max=%lldms\n",
                      animName, xPortGetCoreID(), sumRender / (1000 * OLED_REPORT_FRAMES), maxRender / 1000,
                      sumPeriod / (1000 * (OLED_REPORT_FRAMES - 1)), maxPeriod / 1000);
        sumRender = maxRender = sumPeriod = maxPeriod = 0;
        frames = 0;
    }
}

} // namespace Profile

#define PROFILE_BOOT_BENCH()      Profile::runBootBench()
#define PROFILE_AUDIO_BEGIN()     const int64_t profileAudioT0_ = esp_timer_get_time()
#define PROFILE_AUDIO_END(engine) Profile::audioBlock(esp_timer_get_time() - profileAudioT0_, (engine))
#define PROFILE_OLED_BEGIN()      const int64_t profileOledT0_ = esp_timer_get_time()
#define PROFILE_OLED_END(name)    Profile::oledFrame(profileOledT0_, (name))

#else

#define PROFILE_BOOT_BENCH()      ((void)0)
#define PROFILE_AUDIO_BEGIN()     ((void)0)
#define PROFILE_AUDIO_END(engine) ((void)0)
#define PROFILE_OLED_BEGIN()      ((void)0)
#define PROFILE_OLED_END(name)    ((void)0)

#endif
