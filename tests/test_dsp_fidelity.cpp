// DSP fidelity regression tests: each check pins a measured defect from
// docs/dsp-fidelity-review.md (IDs in the test names).
#include "SynthEngine.h"
#include "Envelope.h"
#include "Oscillator.h"
#include "Filter.h"
#include "SysExProtocol.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Synth;

static const float SR = 44100.0f;
static int g_failures = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static std::vector<float> render(SynthEngine& e, int n) {
    std::vector<float> L(n), R(n);
    e.processBlockFloat(L.data(), R.data(), n);
    return L;
}

static float maxStep(const std::vector<float>& x) {
    float m = 0.0f;
    for (size_t i = 1; i < x.size(); ++i) m = std::max(m, std::abs(x[i] - x[i - 1]));
    return m;
}

static float peak(const std::vector<float>& x, size_t from = 0) {
    float m = 0.0f;
    for (size_t i = from; i < x.size(); ++i) m = std::max(m, std::abs(x[i]));
    return m;
}

static float rms(const std::vector<float>& x, size_t from, size_t to) {
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::sqrt(acc / static_cast<double>(to - from)));
}

// E-01: integrator saturation must be continuous (no single-sample jumps at high resonance)
static void testFilterSaturationIsSmooth() {
    std::printf("E-01 filter saturation continuity\n");
    for (int model = 0; model < 2; ++model) {
        StateVariableFilter svf; svf.init(SR); svf.setCutoff(1000.0f); svf.setResonance(25.0f);
        LadderFilter ladder;     ladder.init(SR); ladder.setCutoff(1000.0f); ladder.setResonance(25.0f);
        Oscillator o; o.init(SR); o.setWaveform(Waveform::Sawtooth); o.setFrequency(110.0f);
        std::vector<float> y(44100);
        for (auto& s : y) {
            const float in = o.process() * 1.5f;
            s = model == 0 ? svf.process(in) : ladder.process(in);
        }
        const float pk = peak(y, 4410);
        // A band-limited signal of this peak can move at most ~pk * 2*pi*fc/fs per sample around fc;
        // allow 1.5x for the saw's own edge content passing through the resonance
        const float bound = 1.5f * pk * 6.2832f * 1000.0f / SR + 0.05f;
        CHECK(maxStep(y) < bound, "%s maxStep=%.3f bound=%.3f", model == 0 ? "SVF" : "Ladder", maxStep(y), bound);
    }
}

// A-01/A-02: ladder self-oscillates at max resonance and thins the passband as resonance rises
static void testLadderCharacter() {
    std::printf("A-01/A-02 ladder self-oscillation and passband loss\n");
    LadderFilter f; f.init(SR); f.setCutoff(1000.0f); f.setResonance(25.0f);
    f.process(0.5f);
    std::vector<float> y(44100);
    for (auto& s : y) s = f.process(0.0f);
    CHECK(peak(y, 22050) > 0.05f, "ringing after 0.5 s = %.4f (expected sustained self-oscillation)", peak(y, 22050));

    auto passband = [](float q) {
        LadderFilter h; h.init(SR); h.setCutoff(2000.0f); h.setResonance(q);
        Oscillator s; s.init(SR); s.setWaveform(Waveform::Triangle); s.setFrequency(50.0f);
        std::vector<float> y(44100);
        for (auto& v : y) v = h.process(0.5f * s.process());
        return peak(y, 22050);
    };
    const float lowQ = passband(0.707f), highQ = passband(15.0f);
    CHECK(highQ < lowQ * 0.85f, "passband low-Q=%.3f high-Q=%.3f (resonance should thin the bass)", lowQ, highQ);
}

// E-02: retriggering a sounding voice must not reset oscillator phase (click)
static void testRetriggerIsClickFree() {
    std::printf("E-02 retrigger click\n");
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::OBXa));
    e.setChorusEnabled(false);
    e.setWaveform(Waveform::Triangle); e.setCutoffHz(20000.0f); e.setFilterEnvAmount(0.0f);
    e.setDetuneCents(0.0f); e.setSubOscLevel(0.0f); e.setDrive(1.0f);
    e.setAmpAttackMs(1.0f); e.setAmpSustainLevel(1.0f); e.setAmpReleaseMs(2000.0f);
    e.setVoiceLimit(1);
    e.noteOn(45, 127); render(e, 22050 + 37);
    const float steady = maxStep(render(e, 4410));
    e.noteOff(45); render(e, 2205 + 91);
    e.noteOn(45, 127);
    const float atRetrigger = maxStep(render(e, 64));
    CHECK(atRetrigger < 2.0f * steady, "retrigger step=%.4f steady=%.4f", atRetrigger, steady);
}

// E-03: mono High priority must not release a key that is still held
static void testMonoPriorityKeepsHeldNote() {
    std::printf("E-03 mono note priority\n");
    for (NotePriority prio : {NotePriority::High, NotePriority::Low}) {
        SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::Minimoog));
        e.setNotePriority(prio);
        const uint8_t kept = prio == NotePriority::High ? 60 : 48;
        const uint8_t other = prio == NotePriority::High ? 48 : 60;
        e.noteOn(kept, 100); render(e, 4410);
        e.noteOn(other, 100); render(e, 4410);
        e.noteOff(other);
        render(e, 44100);
        CHECK(e.getVoice(0).isActive() && !e.getVoice(0).isReleasing() && e.getVoice(0).getNoteNumber() == kept,
              "prio=%d active=%d releasing=%d note=%d", static_cast<int>(prio), e.getVoice(0).isActive(),
              e.getVoice(0).isReleasing(), e.getVoice(0).getNoteNumber());
    }
}

// E-04/A-07: TB-303 slide keeps the envelope running (no retrigger); accent triggers on high velocity
static void testTb303SlideAndAccent() {
    std::printf("E-04/A-07 TB-303 slide and accent\n");
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::TB303));
    e.noteOn(36, 80); render(e, 4410);
    const float before = e.getVoice(0).getEnvelopeLevel();
    e.noteOn(43, 80); // overlapping key = slide
    render(e, 220);
    CHECK(e.getVoice(0).getAmpEnvelopeState() != EnvelopeState::Attack, "envelope retriggered on slide");
    CHECK(e.getVoice(0).getEnvelopeLevel() <= before + 1e-4f, "level rose on slide: %.4f -> %.4f",
          before, e.getVoice(0).getEnvelopeLevel());
    CHECK(e.getVoice(0).getCurrentFreqHz() < e.getVoice(0).getTargetFreqHz() - 1.0f, "slide did not glide");
    e.noteOff(36); e.noteOff(43); render(e, 22050);

    e.noteOn(36, 127); render(e, 64);
    CHECK(e.getVoice(0).isAccented(), "velocity 127 should accent");
    e.noteOff(36); render(e, 22050);
    e.noteOn(36, 60); render(e, 64);
    CHECK(!e.getVoice(0).isAccented(), "velocity 60 should not accent");
}

// E-06: attack reaches peak in the set time (+/-10%)
static void testEnvelopeAttackTiming() {
    std::printf("E-06 envelope attack timing\n");
    for (float t : {10.0f, 100.0f, 1000.0f}) {
        Envelope env; env.init(SR); env.setAttackMs(t); env.setSustainLevel(1.0f);
        env.noteOn();
        int n = 0;
        while (env.getState() == EnvelopeState::Attack && n < 10 * static_cast<int>(SR)) { env.process(); ++n; }
        const float ms = n * 1000.0f / SR;
        CHECK(std::abs(ms - t) < 0.1f * t + 0.1f, "set %.0f ms, measured %.1f ms", t, ms);
    }
    // Sustain changes slew instead of stepping
    Envelope env; env.init(SR); env.setAttackMs(1.0f); env.setDecayMs(5.0f); env.setSustainLevel(1.0f);
    env.noteOn();
    for (int i = 0; i < 4410; ++i) env.process();
    env.setSustainLevel(0.2f);
    const float a = env.process();
    CHECK(a > 0.9f, "sustain stepped to %.3f in one sample", a);
}

// E-07: triangle has no DC offset or clamping in its first cycle
static void testTriangleStartup() {
    std::printf("E-07 triangle startup\n");
    Oscillator o; o.init(SR); o.setWaveform(Waveform::Triangle); o.setFrequency(110.0f);
    const int period = static_cast<int>(SR / 110.0f);
    float mx = -9.0f, mean = 0.0f;
    for (int i = 0; i < period; ++i) { float s = o.process(); mx = std::max(mx, std::abs(s)); mean += s; }
    mean /= static_cast<float>(period);
    CHECK(mx <= 1.02f, "first-cycle peak %.3f", mx);
    CHECK(std::abs(mean) < 0.05f, "first-cycle DC %.3f", mean);
}

// E-08: glide is symmetric in pitch (same time up and down)
static void testGlideSymmetry() {
    std::printf("E-08 glide symmetry\n");
    float times[2];
    for (int dir = 0; dir < 2; ++dir) {
        Voice v; v.init(SR); v.setPortamentoEnabled(true); v.setGlideTimeMs(200.0f); v.setAmpSustainLevel(1.0f);
        const uint8_t from = dir ? 72 : 36, to = dir ? 36 : 72;
        v.noteOn(from, 100, 1);
        for (int i = 0; i < 100; ++i) v.process();
        v.legatoNoteChange(to, 100, true);
        const float midHz = ParameterScaler::midiNoteToHz(54);
        int n = 0;
        while ((dir ? v.getCurrentFreqHz() > midHz : v.getCurrentFreqHz() < midHz) && n < 441000) { v.process(); ++n; }
        times[dir] = n * 1000.0f / SR;
    }
    CHECK(std::abs(times[0] - times[1]) < 0.1f * std::max(times[0], times[1]), "up %.1f ms, down %.1f ms", times[0], times[1]);
}

// E-09: one knob, one parameter
static void testKnobsAreDecoupled() {
    std::printf("E-09 knob decoupling\n");
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::Juno60));
    const float drive = e.getDrive(), detune = e.getDetuneCents();
    e.controlChange(CC_SUB_OSC_LEVEL, 127);
    CHECK(e.getDrive() == drive, "sub knob moved drive %.2f -> %.2f", drive, e.getDrive());
    e.controlChange(CC_CHORUS_MIX, 127);
    CHECK(e.getDetuneCents() == detune, "chorus knob moved detune %.2f -> %.2f", detune, e.getDetuneCents());
    e.controlChange(CC_DRIVE, 127);
    CHECK(e.getDrive() > 3.4f, "drive CC -> %.2f", e.getDrive());
    e.controlChange(CC_DETUNE_DEPTH, 127);
    CHECK(e.getDetuneCents() > 49.0f, "detune CC -> %.2f", e.getDetuneCents());

    // Every knob label in every factory preset must reach a handled CC (label text is what the user sees)
    const auto& presets = getPresets();
    for (size_t i = 0; i < FACTORY_PRESET_COUNT; ++i) {
        for (const KnobBank* bank : {&presets[i].bankA, &presets[i].bankB}) {
            for (int k = 0; k < 8; ++k) {
                const char* l = bank->labels[k];
                const uint8_t cc = bank->ccs[k];
                const bool bad = (std::string(l) == "PolyMod") || (std::string(l) == "Accent" && cc != CC_ACCENT) ||
                                 (std::string(l) == "Drive" && cc != CC_DRIVE) ||
                                 (std::string(l) == "Distort" && cc != CC_DRIVE) ||
                                 (std::string(l) == "Detune" && cc != CC_DETUNE_DEPTH) ||
                                 (std::string(l) == "PolyMode" && cc != CC_VOICE_MODE);
                CHECK(!bad, "preset %s knob %s -> CC %d", presets[i].name, l, cc);
            }
        }
    }
}

// E-11: output stage is linear for normal levels and never exceeds full scale
static void testOutputLimiter() {
    std::printf("E-11 output limiter\n");
    CHECK(ParameterScaler::outputLimiter(0.4f) == 0.4f, "not linear below knee");
    CHECK(ParameterScaler::outputLimiter(10.0f) <= 1.0f, "exceeds full scale");
    CHECK(ParameterScaler::outputLimiter(-10.0f) >= -1.0f, "exceeds full scale (neg)");
    // A single full-velocity voice stays in the linear region
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::OBXa));
    e.setMasterVolume(0.7f);
    e.noteOn(60, 127);
    auto y = render(e, 22050);
    CHECK(peak(y) < 0.6f, "single voice peak %.3f enters the limiter", peak(y));
    CHECK(peak(y) > 0.1f, "single voice peak %.3f too quiet", peak(y));
}

// E-12: legato with a softer note keeps the phrase level
static void testLegatoKeepsLevel() {
    std::printf("E-12 legato velocity\n");
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::CS80)); // velocity-sensitive
    e.setVoiceMode(VoiceMode::Monophonic); e.setLegatoEnabled(true);
    e.setAmpAttackMs(1.0f); e.setAmpSustainLevel(1.0f);
    e.noteOn(48, 127); render(e, 22050);
    const float before = rms(render(e, 4410), 0, 4410);
    e.noteOn(48 + 12, 10);
    const float after = rms(render(e, 4410), 0, 4410);
    CHECK(after > 0.6f * before, "level dropped %.4f -> %.4f on legato", before, after);
}

// A-03 / A-10: key tracking and per-model velocity response
static void testVoicing() {
    std::printf("A-03/A-10 voicing\n");
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::Juno60));
    e.setFilterEnvAmount(0.0f);
    e.noteOn(48, 100); render(e, 64);
    const float low = e.getVoice(0).getEffectiveCutoffHz();
    e.noteOn(84, 100); render(e, 64);
    const float high = e.getVoice(1).getEffectiveCutoffHz();
    CHECK(high > low * 2.5f, "key tracking: C3 %.0f Hz, C6 %.0f Hz", low, high);

    auto loudness = [](SynthModel m, uint8_t vel) {
        SynthEngine x; x.init(SR); x.loadPresetIndex(static_cast<size_t>(m)); x.setChorusEnabled(false);
        x.noteOn(60, vel);
        auto y = render(x, 8820);
        return rms(y, 4410, 8820);
    };
    const float junoRatio = loudness(SynthModel::Juno60, 20) / loudness(SynthModel::Juno60, 127);
    const float fmRatio = loudness(SynthModel::DX_EPiano, 20) / loudness(SynthModel::DX_EPiano, 127);
    CHECK(junoRatio > 0.8f, "Juno should ignore velocity (ratio %.2f)", junoRatio);
    CHECK(fmRatio < 0.5f, "FM should respond to velocity (ratio %.2f)", fmRatio);
}

// E-05: deferred mode leaves voices untouched until the audio thread renders
static void testDeferredEvents() {
    std::printf("E-05 deferred events\n");
    SynthEngine e; e.init(SR); e.setDeferredEvents(true);
    e.noteOn(60, 100);
    CHECK(!e.getVoice(0).isActive(), "voice touched outside the audio thread");
    e.setCutoffHz(500.0f);
    render(e, 16);
    CHECK(e.getActiveVoiceCount() == 1, "note not applied on render");
    CHECK(e.getVoice(0).getEffectiveCutoffHz() < 1500.0f, "cutoff not applied on render (%.0f Hz)",
          e.getVoice(0).getEffectiveCutoffHz());
    e.noteOff(60);
    render(e, 16);
    CHECK(e.getVoice(0).isReleasing(), "note-off not applied on render");
}

// E-17: with the scale quantizer, two keys on one pitch don't release each other
static void testQuantizedNoteOff() {
    std::printf("E-17 quantized note-off\n");
    SynthEngine e; e.init(SR); e.setScaleMode(ScaleMode::Major); e.setRootKey(0);
    e.noteOn(60, 100); // C
    e.noteOn(61, 100); // C# -> snaps to C
    e.noteOff(61);
    int heldC = 0;
    for (size_t i = 0; i < SynthEngine::MAX_VOICES; ++i) {
        const Voice& v = e.getVoice(i);
        if (v.isActive() && !v.isReleasing() && v.getRawNoteNumber() == 60) ++heldC;
    }
    CHECK(heldC == 1, "held C voices = %d", heldC);
}

// Changing key/scale while a key is held must not leave a stuck note (poly and mono)
static void testScaleChangeNoStuckNotes() {
    std::printf("Scale change while holding keys\n");
    for (VoiceMode mode : {VoiceMode::Polyphonic, VoiceMode::Monophonic}) {
        SynthEngine e; e.init(SR); e.setVoiceMode(mode);
        e.setScaleMode(ScaleMode::Major); e.setRootKey(0);
        e.noteOn(61, 100);           // C# -> C in C major
        e.setRootKey(1);             // now C# major: C# is in scale
        e.noteOff(61);
        render(e, 44100);
        CHECK(e.getActiveVoiceCount() == 0 && e.getHeldKeyCount() == 0, "mode=%d active=%zu held=%zu",
              static_cast<int>(mode), e.getActiveVoiceCount(), e.getHeldKeyCount());
    }
}

// E-18: voice mode switch fades instead of hard-cutting
static void testModeSwitchFades() {
    std::printf("E-18 mode switch fade\n");
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::OBXa)); e.setChorusEnabled(false);
    e.noteOn(60, 127); render(e, 22050);
    const float steady = maxStep(render(e, 2205));
    e.setVoiceMode(VoiceMode::Monophonic);
    const float atSwitch = maxStep(render(e, 2205));
    CHECK(atSwitch < 2.0f * steady, "switch step=%.4f steady=%.4f", atSwitch, steady);
}

// E-19: chorus doesn't change loudness at full mix
static void testChorusLevel() {
    std::printf("E-19 chorus level\n");
    StereoChorus c; c.init(SR); c.setEnabled(true); c.setMix(1.0f); c.setRateHz(0.5f); c.setDepthMs(2.0f);
    std::vector<float> L(44100), R(44100);
    // Broadband input below the BBD low-pass (a single sine would sit on a comb-filter peak or
    // notch of the delay; white noise would mostly measure the 9 kHz BBD filter)
    uint32_t seed = 12345u;
    float lp = 0.0f;
    for (int i = 0; i < 44100; ++i) {
        seed = seed * 1664525u + 1013904223u;
        lp += (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f - lp) * 0.25f; // ~2 kHz one-pole
        L[i] = R[i] = lp;
    }
    const float in = rms(L, 22050, 44100);
    c.processBlock(L.data(), R.data(), 44100);
    const float out = rms(L, 22050, 44100);
    CHECK(std::abs(20.0f * std::log10(out / in)) < 1.0f, "chorus level change %.2f dB", 20.0f * std::log10(out / in));
}

// A-12: voice count follows the instrument
static void testVoiceCounts() {
    std::printf("A-12 voice counts\n");
    SynthEngine e; e.init(SR);
    e.loadPresetIndex(static_cast<size_t>(SynthModel::Prophet5));
    CHECK(e.getEffectiveVoiceLimit() == 5, "Prophet-5 voices %zu", e.getEffectiveVoiceLimit());
    e.loadPresetIndex(static_cast<size_t>(SynthModel::OBXa));
    CHECK(e.getEffectiveVoiceLimit() == 8, "OB-Xa voices %zu", e.getEffectiveVoiceLimit());
    e.setVoiceLimit(4);
    CHECK(e.getEffectiveVoiceLimit() == 4, "platform limit ignored");
}

// Voicing survives the legacy v1 48-byte SysEx / NVS patch format (flags bits 4..7), still decoded for old senders and blobs
static void testVoicingRoundTrip() {
    std::printf("Voicing SysEx round-trip\n");
    Preset p = getPresets()[static_cast<size_t>(SynthModel::Minimoog)];
    p.model = SynthModel::User_03;
    p.voicing = SynthModel::Minimoog;
    RawPatchData raw;
    SysExProtocol::presetToRawPatch(p, raw);
    Preset back{};
    char name[16];
    SysExProtocol::rawPatchToPreset(raw, back, name);
    CHECK(back.voicing == SynthModel::Minimoog, "voicing %d", static_cast<int>(back.voicing));
    CHECK((raw.flags & 0x0F) == (p.chorusEnabled ? 1 : 0) + (p.portamentoEnabled ? 2 : 0) + (p.legatoEnabled ? 4 : 0),
          "low flag bits disturbed: 0x%02X", raw.flags);

    // Saving the active factory patch into a user slot records its voicing
    SynthEngine e; e.init(SR); e.loadPresetIndex(static_cast<size_t>(SynthModel::TB303));
    const uint8_t save[] = {0xF0, 0x7D, 0x01, 0x03, USER_SLOT_START, 0xF7};
    e.handleSysEx(save, sizeof(save));
    e.loadPresetIndex(FACTORY_PRESET_COUNT);
    CHECK(e.getVoicingModel() == SynthModel::TB303, "user slot voicing %d", static_cast<int>(e.getVoicingModel()));
    CHECK(e.getVoicing().accent, "user slot lost 303 accent");
}

// Perf refactor guard: rendering a voice in blocks of any size (control ticks and the idle point
// falling mid-block) must equal rendering it one sample at a time with the same pitch-bend stream.
static void testBlockRenderMatchesPerSample() {
    std::printf("Block render == per-sample render\n");
    struct Cfg { EngineMode mode; FilterModel filter; Waveform wave; float drive; float sub; bool accent; float glideMs; };
    const Cfg cfgs[] = {
        {EngineMode::Subtractive, FilterModel::Ladder24, Waveform::Sawtooth, 2.0f, 0.6f, true,  80.0f},
        {EngineMode::Subtractive, FilterModel::Ladder24, Waveform::Square,   1.0f, 0.0f, false, 0.0f},
        {EngineMode::Subtractive, FilterModel::Svf12,    Waveform::Triangle, 1.0f, 0.5f, false, 30.0f},
        {EngineMode::Subtractive, FilterModel::Svf12,    Waveform::Square,   3.0f, 0.0f, false, 0.0f},
        {EngineMode::FM,          FilterModel::Svf12,    Waveform::Sawtooth, 1.0f, 0.0f, false, 0.0f},
        {EngineMode::FM,          FilterModel::Svf12,    Waveform::Sawtooth, 2.5f, 0.0f, false, 50.0f},
    };
    const int N = 20000;
    std::vector<float> bend(N);
    for (int i = 0; i < N; ++i) bend[i] = 0.5f * std::sin(0.0013f * static_cast<float>(i));

    int c = 0;
    for (const Cfg& cfg : cfgs) {
        Voice a;
        a.init(SR);
        a.setEngineMode(cfg.mode);
        a.setFilterModel(cfg.filter);
        a.setWaveform(cfg.wave);
        a.setDrive(cfg.drive);
        a.setSubOscLevel(cfg.sub);
        a.setAccentEnabled(cfg.accent);
        a.setAnalogDrift(0.8f);
        a.setKeyTracking(0.5f);
        a.setResonanceQ(8.0f);
        a.setBaseCutoffHz(900.0f);
        a.setModFeedback(0.7f);
        a.setModRatio(2.0f);
        a.setGlideTimeMs(cfg.glideMs);
        a.setPortamentoEnabled(cfg.glideMs > 0.0f);
        a.setAmpReleaseMs(40.0f);
        a.noteOn(48, 120, 1);
        Voice b = a; // identical state, including the per-voice random stream

        std::vector<float> ya(N, 0.0f), yb(N, 0.0f);
        static const int sizes[] = {128, 37, 1, 8, 77, 128, 3, 64};
        int pos = 0, k = 0;
        bool changed = false, released = false;
        while (pos < N) {
            // Same events at the same sample for both voices
            if (!changed && pos >= 6000) {
                a.legatoNoteChange(55, 90, true); b.legatoNoteChange(55, 90, true); changed = true;
            }
            if (!released && pos >= 12000) { a.noteOff(); b.noteOff(); released = true; }
            const int n = std::min(sizes[k++ % 8], N - pos);
            for (int i = 0; i < n; ++i) {
                a.setPitchBend(bend[pos + i]);
                ya[pos + i] = a.process();
            }
            b.renderBlock(&yb[pos], n, &bend[pos]);
            pos += n;
        }
        int mismatches = 0;
        for (int i = 0; i < N; ++i) if (ya[i] != yb[i]) ++mismatches;
        CHECK(mismatches == 0, "config %d: %d samples differ", c, mismatches);
        CHECK(peak(ya) > 0.01f, "config %d silent", c);
        CHECK(!a.isActive() && !b.isActive(), "config %d: voice should have released to idle", c);
        CHECK(a.getCurrentFreqHz() == b.getCurrentFreqHz(), "config %d: glide state differs", c);
        ++c;
    }

    // SineLut: the audio-path lookup (truncate-and-correct) is bit-identical to the former
    // std::floor version, including negative FM phases (2pi plus a modulation index up to ~20 either way).
    // Plain truncation would stay within 1e-5 of sin(), so compare exactly against the reference.
    std::vector<float> table(SINE_LUT_SIZE);
    for (int i = 0; i < SINE_LUT_SIZE; ++i)
        table[i] = std::sin(static_cast<float>(i) * TWO_PI_F / static_cast<float>(SINE_LUT_SIZE));
    auto floorLookup = [&](float phase) {
        const float pos = phase * (static_cast<float>(SINE_LUT_SIZE) / TWO_PI_F);
        const int idx0 = static_cast<int>(std::floor(pos));
        const float frac = pos - static_cast<float>(idx0);
        const int idx1 = idx0 & (SINE_LUT_SIZE - 1);
        const int idx2 = (idx1 + 1) & (SINE_LUT_SIZE - 1);
        return table[idx1] + frac * (table[idx2] - table[idx1]);
    };
    int lutMismatch = 0;
    for (int i = -40000; i <= 40000; ++i) {
        const float ph = static_cast<float>(i) * 0.000731f;
        if (SineLut::lookupFast(ph) != floorLookup(ph)) ++lutMismatch;
        if (SineLut::lookup(ph) != floorLookup(ph)) ++lutMismatch;
    }
    CHECK(lutMismatch == 0, "%d SineLut lookups differ from the std::floor reference", lutMismatch);
    CHECK(SineLut::lookup(NAN) == 0.0f && SineLut::lookup(INFINITY) == 0.0f && SineLut::lookup(1e9f) == 0.0f,
          "safe lookup must reject non-finite / out-of-range phases");
}

// The division-free filter arithmetic (fastRecip, fastTanhScaled, closed-form ladder stages) must
// reproduce the straightforward formulation it replaced to within float rounding.
static void testFilterArithmeticMatchesReference() {
    float worstRecip = 0.0f, worstSat = 0.0f;
    for (int i = 0; i <= 4000; ++i) {
        const float d = 0.5f * std::pow(2.0f, i * 0.005f);
        worstRecip = std::max(worstRecip, std::abs(ParameterScaler::fastRecip(d) * d - 1.0f));
        const float x = (i - 2000) * 0.015f;
        for (float h : {2.0f, 8.0f}) {
            const float ref = h * ParameterScaler::fastTanh(x / h);
            worstSat = std::max(worstSat, std::abs(ParameterScaler::fastTanhScaled(x, h) - ref) / h);
        }
    }
    CHECK(worstRecip < 1e-6f, "fastRecip relative error %g", worstRecip);
    CHECK(worstSat < 1e-6f, "fastTanhScaled deviates from h*fastTanh(x/h) by %g", worstSat);

    // Reference ladder: the serial cascade of four TPT one-poles after the saturated ZDF solve
    struct RefLadder {
        float s[4]{};
        float process(float in, float G, float k) {
            const float G2 = G * G, G3 = G2 * G, G4 = G2 * G2;
            const float sigma = (1.0f - G) * (G3 * s[0] + G2 * s[1] + G * s[2] + s[3]);
            float x = 2.0f * ParameterScaler::fastTanh(0.5f * (in - k * sigma) / (1.0f + k * G4));
            for (float& st : s) {
                const float v = (x - st) * G;
                const float y = v + st;
                st = y + v;
                x = y;
            }
            if (std::abs(s[3]) < 1e-15f) s[0] = s[1] = s[2] = s[3] = 0.0f;
            return x * (1.0f + 0.5f * k); // LadderFilter's resonance makeup gain
        }
    };
    for (float q : {0.7f, 6.0f, 18.0f}) {
        LadderFilter f;
        f.init(SR);
        f.setResonance(q);
        RefLadder ref;
        Oscillator o;
        o.init(SR);
        o.setWaveform(Waveform::Sawtooth);
        o.setFrequency(110.0f);
        float worst = 0.0f;
        const int n = SR; // 1 s resonant sweep, 60 Hz -> 12 kHz, at a level that drives the saturator
        for (int i = 0; i < n; ++i) {
            const float g = cutoffToG(60.0f * std::pow(200.0f, float(i) / n), SR);
            const float G = g / (1.0f + g);
            f.setNormalizedGain(G);
            const float x = 2.0f * o.process();
            worst = std::max(worst, std::abs(f.process(x) - ref.process(x, G, f.getFeedback())));
        }
        CHECK(worst < 1e-4f, "ladder (Q %.1f) deviates from the serial reference by %g", q, worst);
    }
}

static void testHardSyncSawSweep() {
    std::printf("Subtractive expansions: hard sync saw sweep\n");
    const float masterHz = 220.0f;
    const float masterInc = masterHz / SR;
    const int n = static_cast<int>(SR);

    float mBlep = 0.0f, sBlep = 0.0f;
    float mNaive = 0.0f, sNaive = 0.0f;

    std::vector<float> blepOut(n);
    std::vector<float> naiveOut(n);

    for (int i = 0; i < n; ++i) {
        const float slaveHz = 220.0f + (4000.0f - 220.0f) * (static_cast<float>(i) / static_cast<float>(n));
        const float slaveInc = slaveHz / SR;

        blepOut[i] = process_hard_sync_saw(&mBlep, masterInc, &sBlep, slaveInc);

        mNaive += masterInc;
        sNaive += slaveInc;
        if (sNaive >= 1.0f) sNaive -= 1.0f;
        if (mNaive >= 1.0f) {
            mNaive -= 1.0f;
            const float fracMaster = mNaive / masterInc;
            sNaive = fracMaster * slaveInc;
        }
        naiveOut[i] = (2.0f * sNaive) - 1.0f;

        CHECK(std::isfinite(blepOut[i]), "sample %d non-finite: %f", i, blepOut[i]);
    }

    const float pk = peak(blepOut);
    CHECK(pk <= 1.05f, "hard sync saw peak out of bounds: %.4f", pk);

    double blepDiffSum = 0.0;
    double naiveDiffSum = 0.0;
    for (int i = 1; i < n; ++i) {
        const float db = blepOut[i] - blepOut[i - 1];
        const float dn = naiveOut[i] - naiveOut[i - 1];
        blepDiffSum += db * db;
        naiveDiffSum += dn * dn;
    }
    const float blepDiffRms = static_cast<float>(std::sqrt(blepDiffSum / (n - 1)));
    const float naiveDiffRms = static_cast<float>(std::sqrt(naiveDiffSum / (n - 1)));
    CHECK(blepDiffRms < naiveDiffRms, "BLEP diff RMS %.4f not suppressing aliasing vs naive %.4f",
          blepDiffRms, naiveDiffRms);
}

static void testTubeSaturation() {
    std::printf("Subtractive expansions: tube saturation harmonics and DC offset\n");
    float dcZ1 = 0.0f, dcIn1 = 0.0f;
    const float drive = 2.0f;
    const float bias = 0.5f;
    const int n = static_cast<int>(SR);

    for (int i = 0; i < n; ++i) {
        const float x = std::sin(2.0f * 3.14159265358979323846f * 1000.0f * (static_cast<float>(i) / SR));
        tube_saturate_sample(x, drive, bias, &dcZ1, &dcIn1);
    }

    std::vector<float> y(n);
    double dcSum = 0.0;
    for (int i = 0; i < n; ++i) {
        const float x = std::sin(2.0f * 3.14159265358979323846f * 1000.0f * (static_cast<float>(n + i) / SR));
        y[i] = tube_saturate_sample(x, drive, bias, &dcZ1, &dcIn1);
        dcSum += y[i];
    }

    const float dcOffset = static_cast<float>(std::abs(dcSum / n));
    CHECK(dcOffset < 0.000316f, "DC offset %.6f exceeds -70 dBFS limit", dcOffset);

    auto harmonicMag = [&](int freqHz) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i) {
            const float angle = 2.0f * 3.14159265358979323846f * static_cast<float>(freqHz * i) / SR;
            re += static_cast<double>(y[i]) * std::cos(angle);
            im += static_cast<double>(y[i]) * std::sin(angle);
        }
        re *= 2.0 / n;
        im *= 2.0 / n;
        return static_cast<float>(std::sqrt(re * re + im * im));
    };

    const float h2 = harmonicMag(2000);
    const float h3 = harmonicMag(3000);
    CHECK(h2 > 0.05f, "2nd harmonic missing or too weak: %.4f", h2);
    CHECK(h3 > 0.05f, "3rd harmonic missing or too weak: %.4f", h3);
}

static void testPitchEnvelope808() {
    std::printf("Subtractive expansions: 808 pitch envelope\n");
    PitchEnvelope808 env;
    env.init(SR);
    env.setClickTimeMs(5.0f);
    env.setBodyTimeMs(100.0f);

    env.trigger();
    CHECK(env.isActive(), "envelope should be active after trigger");
    CHECK(env.getStage() == PitchEnvelope808::Stage::Click, "envelope should start in Click stage");

    const float first = env.process();
    CHECK(std::abs(first - 36.0f) < 1e-4f, "initial pitch should be +36 semitones, got %.4f", first);

    float last = first;
    int samplesToSeven = 0;
    const int maxClickSamples = static_cast<int>(15.0f * 0.001f * SR);
    for (int i = 1; i < maxClickSamples; ++i) {
        float val = env.process();
        CHECK(val <= last, "pitch should decay monotonically during click: %f > %f", val, last);
        last = val;
        if (env.getStage() == PitchEnvelope808::Stage::Body) {
            samplesToSeven = i;
            break;
        }
    }

    const float clickDurationMs = static_cast<float>(samplesToSeven) * 1000.0f / SR;
    CHECK(clickDurationMs >= 4.0f && clickDurationMs <= 6.0f,
          "click stage reached +7 semitones in %.2f ms (expected ~5 ms)", clickDurationMs);

    int samplesToZero = 0;
    const int maxBodySamples = static_cast<int>(300.0f * 0.001f * SR);
    for (int i = 0; i < maxBodySamples; ++i) {
        float val = env.process();
        CHECK(val <= last, "pitch should decay monotonically during body: %f > %f", val, last);
        last = val;
        if (!env.isActive()) {
            samplesToZero = i;
            break;
        }
    }

    CHECK(!env.isActive(), "envelope should be idle after body decay");
    CHECK(env.process() == 0.0f, "envelope should output 0 semitones when idle");

    const float bodyDurationMs = static_cast<float>(samplesToZero) * 1000.0f / SR;
    CHECK(bodyDurationMs >= 70.0f && bodyDurationMs <= 130.0f,
          "body stage settled in %.2f ms (expected ~100 ms)", bodyDurationMs);
}

static void testRingModAndCrossMod() {
    std::printf("Subtractive expansions: ring modulation and cross modulation\n");
    float dcZ1 = 0.0f, dcIn1 = 0.0f;
    for (int i = 0; i < 44100; ++i) {
        float s1 = std::sin(2.0f * 3.14159265f * 440.0f * i / SR);
        float s2 = std::sin(2.0f * 3.14159265f * 330.0f * i / SR);
        float out = ring_modulate(s1, s2, &dcZ1, &dcIn1);
        CHECK(std::isfinite(out), "ring mod non-finite: %f", out);
    }

    const float baseInc = 0.01f;
    const float mod = cross_modulate(baseInc, 1.0f, 0.5f);
    CHECK(mod > baseInc, "positive mod should increase phase inc: %f", mod);
    const float clamped = cross_modulate(baseInc, 100.0f, 10.0f);
    CHECK(clamped == 0.48f, "cross mod should clamp to 0.48: %f", clamped);
    const float clampedMin = cross_modulate(baseInc, -100.0f, 10.0f);
    CHECK(clampedMin == 0.0f, "cross mod should clamp to 0.0: %f", clampedMin);
}

int main() {
    testHardSyncSawSweep();
    testTubeSaturation();
    testPitchEnvelope808();
    testRingModAndCrossMod();
    testFilterArithmeticMatchesReference();
    testBlockRenderMatchesPerSample();
    testFilterSaturationIsSmooth();
    testLadderCharacter();
    testRetriggerIsClickFree();
    testMonoPriorityKeepsHeldNote();
    testTb303SlideAndAccent();
    testEnvelopeAttackTiming();
    testTriangleStartup();
    testGlideSymmetry();
    testKnobsAreDecoupled();
    testOutputLimiter();
    testLegatoKeepsLevel();
    testVoicing();
    testDeferredEvents();
    testQuantizedNoteOff();
    testScaleChangeNoStuckNotes();
    testModeSwitchFades();
    testChorusLevel();
    testVoiceCounts();
    testVoicingRoundTrip();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All DSP fidelity checks passed\n");
    return 0;
}
