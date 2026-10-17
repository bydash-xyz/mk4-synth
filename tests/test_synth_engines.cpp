// Wavetable, phase distortion, pluck and additive engines: pitch, band-limiting, the timbre controls each
// engine exposes, patch round-trips and the bank layout they share with the older engines.
#include "SynthEngine.h"
#include "SysExProtocol.h"
#include "Wavetables.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
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

static float noteHz(int note) { return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f); }

// A voice set up for one engine with flat envelopes, no drift, no detune and no velocity scaling
static Voice makeVoice(EngineMode mode, const TimbreParams& t) {
    Voice v;
    v.init(SR);
    v.setEngineMode(mode);
    v.setFilterModel(FilterModel::Svf12);
    v.setBaseCutoffHz(20000.0f);
    v.setResonanceQ(0.5f);
    v.setFilterEnvAmount(0.0f);
    v.setDetuneCents(0.0f);
    v.setAnalogDrift(0.0f);
    v.setVelocityToAmp(0.0f);
    v.setOscKeySync(true);
    v.setAmpAttackMs(1.0f);
    v.setAmpSustainLevel(1.0f);
    v.setAmpReleaseMs(20.0f);
    v.setFilterAttackMs(1.0f);
    v.setFilterDecayMs(200.0f);
    v.setFilterSustainLevel(0.0f);
    v.setTimbre(t);
    return v;
}

static std::vector<float> renderVoice(Voice& v, int note, int n) {
    std::vector<float> y(n, 0.0f), bend(n, 0.0f);
    v.noteOn(static_cast<uint8_t>(note), 100, 1);
    for (int pos = 0; pos < n; pos += 128) v.renderBlock(&y[pos], std::min(128, n - pos), &bend[pos]);
    return y;
}

// Power spectrum (4-term Blackman-Harris window: sidelobes -92 dB, zero-padded) of y[from, from + len)
static std::vector<double> spectrum(const std::vector<float>& y, int from, int len, int fftSize) {
    std::vector<double> re(fftSize, 0.0), im(fftSize, 0.0);
    for (int i = 0; i < len; ++i) {
        const double t = 2.0 * M_PI * i / (len - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        re[i] = y[from + i] * w;
    }
    for (int i = 1, j = 0; i < fftSize; ++i) { // iterative radix-2 FFT
        int bit = fftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len2 = 2; len2 <= fftSize; len2 <<= 1) {
        const double ang = -2.0 * M_PI / len2;
        for (int i = 0; i < fftSize; i += len2) {
            for (int k = 0; k < len2 / 2; ++k) {
                const double c = std::cos(ang * k), s = std::sin(ang * k);
                const double xr = re[i + k + len2 / 2] * c - im[i + k + len2 / 2] * s;
                const double xi = re[i + k + len2 / 2] * s + im[i + k + len2 / 2] * c;
                re[i + k + len2 / 2] = re[i + k] - xr; im[i + k + len2 / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
        }
    }
    std::vector<double> p(fftSize / 2);
    for (int k = 0; k < fftSize / 2; ++k) p[k] = re[k] * re[k] + im[k] * im[k];
    return p;
}

struct SpectrumStats {
    double peakHz{0.0};      // strongest bin within +-6 % of the expected fundamental (interpolated)
    double inharmonicDb{0.0};// energy away from multiples of f0, relative to the total
    double centroidHz{0.0};
};

static SpectrumStats analyse(const std::vector<float>& y, float f0, int from = 4096, int len = 32768) {
    len = std::min(len, static_cast<int>(y.size()) - from);
    const int fft = 131072;
    const auto p = spectrum(y, from, len, fft);
    const double binHz = SR / fft;
    SpectrumStats s;
    const int k0 = static_cast<int>(f0 * 0.94 / binHz), k1 = static_cast<int>(f0 * 1.06 / binHz);
    int kp = k0;
    for (int k = k0; k <= k1; ++k) if (p[k] > p[kp]) kp = k;
    const double a = std::log(p[kp - 1] + 1e-30), b = std::log(p[kp] + 1e-30), c = std::log(p[kp + 1] + 1e-30);
    s.peakHz = (kp + 0.5 * (a - c) / (a - 2 * b + c)) * binHz;
    double total = 0.0, off = 0.0, wsum = 0.0;
    for (int k = 1; k < fft / 2; ++k) {
        const double hz = k * binHz;
        const double h = hz / f0;
        total += p[k];
        wsum += p[k] * hz;
        if (std::abs(h - std::round(h)) * f0 > 15.0) off += p[k]; // main lobe: +-4 bins of 1.35 Hz
    }
    s.inharmonicDb = 10.0 * std::log10(off / total + 1e-15);
    s.centroidHz = wsum / total;
    return s;
}

static double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

static void testPitchAndBandLimit() {
    std::printf("Engines: pitch accuracy and band-limiting\n");
    struct Case { const char* name; EngineMode mode; TimbreParams t; };
    TimbreParams wt;   wt.wtTable = 1; wt.wtPosition = 0.0f; wt.wtEnvAmount = 0.0f; // band-limited saw
    TimbreParams wtG;  wtG.wtTable = 3; wtG.wtPosition = 1.0f; wtG.wtEnvAmount = 0.0f; // brightest table
    TimbreParams pd;   pd.pdWave = PdWave::Saw; pd.pdDcw = 0.9f;
    TimbreParams pdR;  pdR.pdWave = PdWave::ResoTrap; pdR.pdDcw = 1.0f;
    TimbreParams ks;   ks.pluckDecayMs = 6000.0f; ks.pluckBright = 0.6f;
    TimbreParams org;  std::fill(std::begin(org.drawbars), std::end(org.drawbars), 0.0f);
    org.drawbars[2] = 8; org.drawbars[3] = 8; org.drawbars[5] = 8; org.drawbars[8] = 8; org.keyClick = 0.0f;
    const Case cases[] = {
        {"wavetable saw", EngineMode::Wavetable, wt},  {"wavetable glass", EngineMode::Wavetable, wtG},
        {"pd saw", EngineMode::PhaseDist, pd},          {"pd reso", EngineMode::PhaseDist, pdR},
        {"pluck", EngineMode::Pluck, ks},               {"additive", EngineMode::Additive, org},
    };
    for (const Case& c : cases) {
        for (int note : {36, 48, 60, 72, 84, 96, 103}) {
            Voice v = makeVoice(c.mode, c.t);
            const auto y = renderVoice(v, note, 4096 + 32768);
            const float f0 = noteHz(note);
            const SpectrumStats s = analyse(y, f0);
            // Plucked strings: the allpass and loop filter phase delays are exact at the fundamental only
            const double tol = (c.mode == EngineMode::Pluck) ? 3.0 : 0.5;
            CHECK(std::abs(cents(s.peakHz, f0)) < tol, "%s note %d: %.2f cents off", c.name, note,
                  cents(s.peakHz, f0));
            // Aliases land between the harmonics; band-limited engines keep them 50 dB down. The plucked
            // string is skipped: its partials are slightly stretched (loop filter dispersion), not aliased.
            // The CZ resonance windows keep their hard corners (a 12 dB/oct tail), so from C6 up they are
            // only held to 30 dB.
            const bool resoTop = c.mode == EngineMode::PhaseDist && c.t.pdWave >= PdWave::ResoSaw && note > 72;
            const double limit = resoTop ? -30.0 : -50.0;
            if (c.mode != EngineMode::Pluck) {
                CHECK(s.inharmonicDb < limit, "%s note %d: inharmonic energy %.1f dB", c.name, note, s.inharmonicDb);
            }
        }
    }
}

static void testWavetableMorph() {
    std::printf("Wavetable: frame position and envelope sweep the spectrum\n");
    CHECK(Wavetables::isReady(), "tables not built");
    double prev = 0.0;
    for (int f = 0; f < Wavetables::FRAMES; f += 2) {
        TimbreParams t; t.wtTable = 0; t.wtPosition = static_cast<float>(f) / (Wavetables::FRAMES - 1); t.wtEnvAmount = 0.0f;
        Voice v = makeVoice(EngineMode::Wavetable, t);
        const auto y = renderVoice(v, 45, 24576);
        const double centroid = analyse(y, noteHz(45)).centroidHz;
        CHECK(centroid > prev * 1.15, "PPG sweep frame %d: centroid %.0f Hz not above %.0f Hz", f, centroid, prev);
        prev = centroid;
    }
    // Envelope -> position: an attack-only envelope (sustain 0) starts bright and settles dark
    TimbreParams t; t.wtTable = 0; t.wtPosition = 0.0f; t.wtEnvAmount = 1.0f;
    Voice v = makeVoice(EngineMode::Wavetable, t);
    v.setFilterDecayMs(150.0f);
    const auto y = renderVoice(v, 45, 44100);
    const double early = analyse(y, noteHz(45), 256, 2048).centroidHz;
    const double late = analyse(y, noteHz(45), 30000, 8192).centroidHz;
    CHECK(early > late * 2.0, "envelope sweep: early centroid %.0f Hz vs late %.0f Hz", early, late);
}

static void testPhaseDistortionDcw() {
    std::printf("Phase distortion: DCW 0 is a sine, more DCW adds harmonics\n");
    for (uint8_t w = 0; w < static_cast<uint8_t>(PdWave::Count); ++w) {
        double prev = -1.0;
        for (float dcw : {0.0f, 0.5f, 0.95f}) {
            TimbreParams t; t.pdWave = static_cast<PdWave>(w); t.pdDcw = dcw;
            Voice v = makeVoice(EngineMode::PhaseDist, t);
            const auto y = renderVoice(v, 45, 24576);
            const double centroid = analyse(y, noteHz(45)).centroidHz;
            if (dcw == 0.0f && w <= static_cast<uint8_t>(PdWave::DoubleSine)) {
                // Undistorted phase reads a pure cosine: everything sits on the fundamental
                CHECK(centroid < noteHz(45) * 1.05, "wave %d at DCW 0: centroid %.0f Hz (expected a sine)", w, centroid);
            }
            CHECK(centroid > prev, "wave %d: DCW %.2f does not brighten (%.0f Hz <= %.0f Hz)", w, dcw, centroid, prev);
            prev = centroid;
        }
    }
}

static void testPluckDecayAndBrightness() {
    std::printf("Pluck: string decay follows the T60 setting, brightness sets the tone\n");
    for (float t60 : {800.0f, 3000.0f}) {
        TimbreParams t; t.pluckDecayMs = t60; t.pluckBright = 0.5f;
        Voice v = makeVoice(EngineMode::Pluck, t);
        const auto y = renderVoice(v, 45, static_cast<int>(SR * 2.0f));
        // Level of the fundamental 100 ms in and 100 ms + t60/3 later should fall about 20 dB
        auto level = [&](int at) {
            const auto p = spectrum(y, at, 4096, 16384);
            const int k = static_cast<int>(noteHz(45) / (SR / 16384) + 0.5);
            return 10.0 * std::log10(std::max({p[k - 1], p[k], p[k + 1]}) + 1e-30);
        };
        const int a = static_cast<int>(0.1f * SR);
        const int b = a + static_cast<int>(t60 / 3.0f * 0.001f * SR);
        const double drop = level(a) - level(b);
        CHECK(drop > 15.0 && drop < 25.0, "T60 %.0f ms: fundamental fell %.1f dB over T60/3 (expected ~20)", t60, drop);
    }
    double dark = 0.0, bright = 0.0;
    for (float b : {0.1f, 0.9f}) {
        TimbreParams t; t.pluckBright = b;
        Voice v = makeVoice(EngineMode::Pluck, t);
        const auto y = renderVoice(v, 52, 24576);
        (b < 0.5f ? dark : bright) = analyse(y, noteHz(52), 1024, 8192).centroidHz;
    }
    CHECK(bright > dark * 1.5, "brightness: centroid %.0f Hz (bright) vs %.0f Hz (dark)", bright, dark);
    // Releasing the key mutes the string: the voice goes idle after the amp release
    TimbreParams t;
    Voice v = makeVoice(EngineMode::Pluck, t);
    renderVoice(v, 60, 4096);
    v.noteOff();
    std::vector<float> y(8192, 0.0f), bend(8192, 0.0f);
    v.renderBlock(y.data(), 8192, bend.data());
    CHECK(!v.isActive(), "pluck voice still active after release");
}

static void testDrawbars() {
    std::printf("Additive: drawbars put energy on their footage harmonics\n");
    // 8' alone: a sine at the key's pitch. 16' alone: an octave below. 2 2/3': the twelfth above. 1': three octaves up.
    struct Bar { int index; float ratio; };
    for (const Bar b : {Bar{2, 1.0f}, Bar{0, 0.5f}, Bar{4, 3.0f}, Bar{8, 8.0f}}) {
        TimbreParams t; std::fill(std::begin(t.drawbars), std::end(t.drawbars), 0.0f);
        t.drawbars[b.index] = 8; t.keyClick = 0.0f;
        Voice v = makeVoice(EngineMode::Additive, t);
        const auto y = renderVoice(v, 57, 24576);
        const float f = noteHz(57) * b.ratio;
        const SpectrumStats s = analyse(y, f);
        CHECK(std::abs(cents(s.peakHz, f)) < 0.5, "drawbar %d: peak %.1f Hz, expected %.1f Hz", b.index, s.peakHz, f);
        CHECK(std::abs(s.centroidHz - f) < f * 0.02, "drawbar %d: not a pure partial (centroid %.1f Hz)", b.index, s.centroidHz);
    }
    // Percussion: the third harmonic (2 2/3') decays under the filter envelope while the 8' holds
    TimbreParams t; std::fill(std::begin(t.drawbars), std::end(t.drawbars), 0.0f);
    t.drawbars[2] = 8; t.percLevel = 1.0f; t.percHarmonic = 1; t.keyClick = 0.0f;
    Voice v = makeVoice(EngineMode::Additive, t);
    const auto y = renderVoice(v, 57, static_cast<int>(SR));
    auto third = [&](int at) {
        const auto p = spectrum(y, at, 4096, 16384);
        const int k = static_cast<int>(3.0f * noteHz(57) / (SR / 16384) + 0.5);
        return std::max({p[k - 1], p[k], p[k + 1]});
    };
    CHECK(third(200) > third(30000) * 1000.0, "percussion did not decay (%.3g -> %.3g)", third(200), third(30000));
}

static void testEngineVoicesAreBlockInvariant() {
    std::printf("Engines: block render == per-sample render\n");
    const EngineMode modes[] = {EngineMode::Wavetable, EngineMode::PhaseDist, EngineMode::Pluck, EngineMode::Additive};
    for (EngineMode mode : modes) {
        TimbreParams t; t.wtTable = 2; t.pdWave = PdWave::ResoSaw; t.percLevel = 0.7f;
        Voice a = makeVoice(mode, t);
        a.setDetuneCents(7.0f);
        a.setFilterEnvAmount(0.6f);
        a.setAnalogDrift(0.5f);
        a.setDrive(1.8f);
        a.setGlideTimeMs(40.0f);
        a.setPortamentoEnabled(true);
        a.noteOn(50, 110, 1);
        Voice b = a;
        const int N = 12000;
        std::vector<float> ya(N, 0.0f), yb(N, 0.0f), bend(N);
        for (int i = 0; i < N; ++i) bend[i] = 0.3f * std::sin(0.002f * static_cast<float>(i));
        static const int sizes[] = {128, 37, 1, 8, 77, 3};
        int pos = 0, k = 0;
        bool changed = false;
        while (pos < N) {
            if (!changed && pos >= 5000) { a.legatoNoteChange(57, 90, true); b.legatoNoteChange(57, 90, true); changed = true; }
            const int n = std::min(sizes[k++ % 6], N - pos);
            for (int i = 0; i < n; ++i) { a.setPitchBend(bend[pos + i]); ya[pos + i] = a.process(); }
            b.renderBlock(&yb[pos], n, &bend[pos]);
            pos += n;
        }
        int mismatches = 0;
        for (int i = 0; i < N; ++i) if (ya[i] != yb[i]) ++mismatches;
        CHECK(mismatches == 0, "%s: %d samples differ", SynthEngine::getEngineModeName(mode), mismatches);
    }
}

static void testFactoryPresetsAndKnobs() {
    std::printf("Engines: factory presets, knob CCs and display text\n");
    auto e = std::make_unique<SynthEngine>();
    e->init(SR);
    struct P { SynthModel model; EngineMode mode; };
    const P presets[] = {
        {SynthModel::PPG_Wave, EngineMode::Wavetable}, {SynthModel::WT_VoxPad, EngineMode::Wavetable},
        {SynthModel::CZ_Bass, EngineMode::PhaseDist},  {SynthModel::CZ_Reso, EngineMode::PhaseDist},
        {SynthModel::KS_Guitar, EngineMode::Pluck},    {SynthModel::KS_Koto, EngineMode::Pluck},
        {SynthModel::B3_Jazz, EngineMode::Additive},   {SynthModel::B3_Gospel, EngineMode::Additive},
    };
    for (const P& p : presets) {
        e->loadPreset(p.model);
        CHECK(e->getEngineMode() == p.mode, "%s loads engine %s", getPresets()[static_cast<size_t>(p.model)].name,
              SynthEngine::getEngineModeName(e->getEngineMode()));
        CHECK(e->getVoicingModel() == p.model, "%s voicing", getPresets()[static_cast<size_t>(p.model)].name);
        CHECK(isFactorySlot(static_cast<uint8_t>(p.model) + 1), "%s is not in a factory slot",
              getPresets()[static_cast<size_t>(p.model)].name);
        // Every knob of both banks has a display name and value
        for (const KnobBank* bank : {&e->getCurrentPreset().bankA, &e->getCurrentPreset().bankB}) {
            for (size_t k = 0; k < 8; ++k) {
                std::string label, value;
                CHECK(e->describeParameter(bank->ccs[k], bank->defaults[k], label, value) && !value.empty(),
                      "%s knob %s has no display text", getPresets()[static_cast<size_t>(p.model)].name, bank->labels[k]);
            }
        }
    }

    e->loadPreset(SynthModel::PPG_Wave);
    e->controlChange(CC_WT_POSITION, 127);
    e->controlChange(CC_WT_TABLE, 127);
    CHECK(e->getTimbre().wtPosition == 1.0f && e->getTimbre().wtTable == WAVETABLE_COUNT - 1, "wavetable CCs");
    e->loadPreset(SynthModel::CZ_Bass);
    e->controlChange(CC_PD_WAVE, 127);
    e->controlChange(CC_PD_DCW, 0);
    CHECK(e->getTimbre().pdWave == PdWave::ResoTrap && e->getTimbre().pdDcw == 0.0f, "phase distortion CCs");
    e->loadPreset(SynthModel::KS_Guitar);
    e->controlChange(CC_PLUCK_DECAY, 127);
    CHECK(std::abs(e->getTimbre().pluckDecayMs - 10000.0f) < 1.0f, "pluck decay CC");
    e->loadPreset(SynthModel::B3_Jazz);
    e->controlChange(CC_DRAWBAR_9, 70); // 70/127 * 8 = 4.4 -> drawbar step 4
    e->controlChange(CC_PERC_HARMONIC, 0);
    CHECK(e->getTimbre().drawbars[8] == 4.0f && e->getTimbre().percHarmonic == 0, "drawbar / percussion CCs");
    std::string label, value;
    e->describeParameter(CC_DRAWBAR_9, 70, label, value);
    CHECK(value == "4", "drawbar display '%s'", value.c_str());

    // Engines play through the full engine (chorus, tremolo, voice allocation) without blowing up
    for (const P& p : presets) {
        e->loadPreset(p.model);
        for (int n : {48, 52, 55, 60}) e->noteOn(static_cast<uint8_t>(n), 110);
        std::vector<float> L(22050), R(22050);
        e->processBlockFloat(L.data(), R.data(), L.size());
        float pk = 0.0f;
        bool finite = true;
        for (float s : L) { pk = std::max(pk, std::abs(s)); finite = finite && std::isfinite(s); }
        CHECK(finite && pk > 0.01f && pk <= 1.0f, "%s chord: peak %.3f finite %d",
              getPresets()[static_cast<size_t>(p.model)].name, pk, finite);
        e->allNotesOff();
        e->processBlockFloat(L.data(), R.data(), L.size());
    }
}

static void testPatchRoundTrip() {
    std::printf("Engines: PatchDumpV2 carries the engine and its parameters\n");
    for (SynthModel m : {SynthModel::PPG_Wave, SynthModel::CZ_Reso, SynthModel::KS_Koto, SynthModel::B3_Gospel}) {
        const Preset& src = getPresets()[static_cast<size_t>(m)];
        uint8_t body[MAX_PATCH_BODY_SIZE];
        const size_t len = SysExProtocol::encodePatchBody(src, body, sizeof(body));
        CHECK(len > 0 && len <= MAX_PATCH_BODY_SIZE, "%s encodes (%zu bytes)", src.name, len);
        Preset dst = makeDefaultUserPreset(SynthModel::User_01, "USER 01", "");
        char name[16];
        CHECK(SysExProtocol::decodePatchBody(body, len, dst, name), "%s decodes", src.name);
        CHECK(presetEngineMode(dst) == presetEngineMode(src), "%s engine mode", src.name);
        CHECK(resolveVoicingModel(dst) == m, "%s voicing", src.name);
        const TimbreParams& a = src.timbre;
        const TimbreParams& b = dst.timbre;
        bool same = a.wtTable == b.wtTable && a.pdWave == b.pdWave && a.percHarmonic == b.percHarmonic &&
                    std::abs(a.wtPosition - b.wtPosition) < 1e-3f && std::abs(a.pdDcw - b.pdDcw) < 1e-3f &&
                    std::abs(a.pluckDecayMs - b.pluckDecayMs) / a.pluckDecayMs < 1e-3f &&
                    std::abs(a.pluckPick - b.pluckPick) < 1e-3f && std::abs(a.keyClick - b.keyClick) < 1e-3f;
        for (size_t i = 0; i < DRAWBAR_COUNT; ++i) same = same && std::abs(a.drawbars[i] - b.drawbars[i]) < 1e-3f;
        CHECK(same, "%s timbre parameters differ after the round-trip", src.name);
    }

    // FastParamUpdate: EngineMode takes the engine index; legacy senders' 1 still means FM
    auto e = std::make_unique<SynthEngine>();
    e->init(SR);
    uint8_t msg[16];
    size_t len = SysExProtocol::encodeFastParamUpdate(ParamId::EngineMode, 4, 0, msg, sizeof(msg));
    e->handleSysEx(msg, len);
    CHECK(e->getEngineMode() == EngineMode::Pluck, "FastParamUpdate engine 4 -> %s", SynthEngine::getEngineModeName(e->getEngineMode()));
    len = SysExProtocol::encodeFastParamUpdate(ParamId::EngineMode, 1, 0, msg, sizeof(msg));
    e->handleSysEx(msg, len);
    CHECK(e->getEngineMode() == EngineMode::FM, "FastParamUpdate engine 1 -> %s", SynthEngine::getEngineModeName(e->getEngineMode()));
    const ParamDef* bar = SysExProtocol::findPatchParam(ParamId::Drawbar1);
    len = SysExProtocol::encodeFastParamUpdate(ParamId::Drawbar1, SysExProtocol::paramToValue14(*bar, 6.0f), 0, msg, sizeof(msg));
    e->handleSysEx(msg, len);
    CHECK(std::abs(e->getTimbre().drawbars[0] - 6.0f) < 1e-3f, "FastParamUpdate drawbar 16' = %.2f", e->getTimbre().drawbars[0]);
}

static void testLegacyV1Models() {
    std::printf("Bank layout: v1 dumps from the 17-user-slot era stay user patches\n");
    // Model byte 20 was User_06 when v1 was written; today index 20 is the KS KOTO factory model
    RawPatchData raw{};
    std::strncpy(raw.name, "OLD USER", sizeof(raw.name) - 1);
    raw.model = 20;
    raw.flags = 0;
    Preset p = makeDefaultUserPreset(SynthModel::User_01, "USER 01", "");
    char name[16];
    SysExProtocol::rawPatchToPreset(raw, p, name);
    CHECK(p.model == SynthModel::User, "v1 model 20 decoded as model %d", static_cast<int>(p.model));
    CHECK(presetEngineMode(p) == EngineMode::Subtractive, "v1 user patch engine %d", static_cast<int>(presetEngineMode(p)));

    CHECK(FACTORY_SLOT_END + 1 == USER_SLOT_START && USER_SLOT_END == TOTAL_SLOT_COUNT, "slot ranges");
    CHECK(FACTORY_PRESET_COUNT + USER_PRESET_COUNT == TOTAL_PRESET_COUNT, "preset counts");
    for (size_t i = 0; i < TOTAL_PRESET_COUNT; ++i) {
        CHECK(static_cast<size_t>(getPresets()[i].model) == i, "preset %zu has model %d", i,
              static_cast<int>(getPresets()[i].model));
        CHECK(isUserModel(getPresets()[i].model) == (i >= FACTORY_PRESET_COUNT), "preset %zu user flag", i);
    }
}

static float measureToneGain(SolinaEnsemble& ensemble, float freqHz) {
    ensemble.reset();
    float sumIn2 = 0.0f;
    float sumOut2 = 0.0f;
    for (int i = 0; i < 512; ++i) {
        float in = std::sin(2.0f * M_PI * freqHz * i / SR);
        float outL = 0.0f, outR = 0.0f;
        ensemble.processSample(in, in, outL, outR);
        if (i >= 256) {
            sumIn2 += in * in;
            sumOut2 += outL * outL;
        }
    }
    return std::sqrt(sumOut2 / (sumIn2 + 1e-9f));
}

static void testOpx4AndSolina() {
    std::printf("OPx-4 4-Operator FM synthesis & Solina Ensemble checks\n");

    for (int algo = 0; algo < 8; ++algo) {
        Opx4Engine opx;
        opx.init(SR);
        opx.setAlgorithm(algo);
        opx.setFrequency(440.0f);
        opx.setOpRatio(0, 1.0f);
        opx.setOpRatio(1, 1.0f);
        opx.setOpRatio(2, 2.0f);
        opx.setOpRatio(3, 3.0f);
        opx.setOpLevel(0, 1.0f);
        opx.setOpLevel(1, 1.0f);
        opx.setOpLevel(2, 1.0f);
        opx.setOpLevel(3, 1.0f);

        std::vector<float> block(256);
        opx.renderBlock(block.data(), 256);
        float energy = 0.0f;
        for (float s : block) energy += s * s;
        CHECK(energy > 0.5f, "OPx-4 algo %d produces signal (energy=%.2f)", algo, energy);
    }

    {
        Opx4Engine opx7;
        opx7.init(SR);
        opx7.setAlgorithm(7);
        opx7.setFrequency(440.0f);
        opx7.setOpRatio(0, 1.0f);
        opx7.setOpRatio(1, 2.0f);
        opx7.setOpRatio(2, 3.0f);
        opx7.setOpRatio(3, 4.0f);
        opx7.setOpLevel(0, 0.5f);
        opx7.setOpLevel(1, 0.5f);
        opx7.setOpLevel(2, 0.5f);
        opx7.setOpLevel(3, 0.5f);
        float s = opx7.processSample();
        CHECK(std::abs(s) < 1e-4f, "OPx-4 algo 7 start sample near zero (%.4f)", s);
    }

    {
        Opx4Engine opxNoFb, opxFb;
        opxNoFb.init(SR);
        opxFb.init(SR);
        opxNoFb.setAlgorithm(0);
        opxFb.setAlgorithm(0);
        opxNoFb.setFrequency(220.0f);
        opxFb.setFrequency(220.0f);
        opxNoFb.setFeedback(0.0f);
        opxFb.setFeedback(2.0f);

        bool diffDetected = false;
        bool stable = true;
        for (int i = 0; i < 512; ++i) {
            float s0 = opxNoFb.processSample();
            float s1 = opxFb.processSample();
            if (std::abs(s0 - s1) > 0.05f) diffDetected = true;
            if (!std::isfinite(s1) || std::abs(s1) > 10.0f) stable = false;
        }
        CHECK(diffDetected, "OPx-4 feedback changes output spectrum");
        CHECK(stable, "OPx-4 feedback remains numerically stable");
    }

    {
        SolinaEnsemble solina;
        solina.init(SR);
        solina.setFormantMode(SolinaFormant::Bypass);
        solina.setMix(1.0f);

        std::vector<float> l(1024), r(1024);
        for (int i = 0; i < 1024; ++i) {
            float s = std::sin(2.0f * M_PI * 440.0f * i / SR);
            l[i] = s;
            r[i] = s;
        }
        solina.processBlock(l.data(), r.data(), 1024);

        float dot = 0.0f, normL = 0.0f, normR = 0.0f;
        for (int i = 400; i < 1024; ++i) {
            dot += l[i] * r[i];
            normL += l[i] * l[i];
            normR += r[i] * r[i];
        }
        float corr = dot / (std::sqrt(normL * normR) + 1e-9f);
        CHECK(corr < 0.85f, "Solina stereo decorrelation from 3-phase BBD matrix (corr=%.3f)", corr);
        CHECK(normL > 1.0f && normR > 1.0f, "Solina outputs active on both channels");
    }

    {
        SolinaEnsemble solinaHorn;
        solinaHorn.init(SR);
        solinaHorn.setFormantMode(SolinaFormant::Horn);
        float gain550 = measureToneGain(solinaHorn, 550.0f);
        float gain100 = measureToneGain(solinaHorn, 100.0f);
        CHECK(gain550 > gain100 * 1.5f, "Solina Horn formant boosts 550 Hz (gain=%.2f vs %.2f)", gain550, gain100);

        SolinaEnsemble solinaViolin;
        solinaViolin.init(SR);
        solinaViolin.setFormantMode(SolinaFormant::Violin);
        float gain320 = measureToneGain(solinaViolin, 320.0f);
        float gain100v = measureToneGain(solinaViolin, 100.0f);
        CHECK(gain320 > gain100v * 1.5f, "Solina Violin formant boosts 320 Hz (gain=%.2f vs %.2f)", gain320, gain100v);
    }
}

int main() {
    testPitchAndBandLimit();
    testWavetableMorph();
    testPhaseDistortionDcw();
    testPluckDecayAndBrightness();
    testDrawbars();
    testEngineVoicesAreBlockInvariant();
    testFactoryPresetsAndKnobs();
    testPatchRoundTrip();
    testLegacyV1Models();
    testOpx4AndSolina();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All synthesis engine checks passed\n");
    return 0;
}
