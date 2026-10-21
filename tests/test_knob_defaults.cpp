// Knob default drift test.
//
// Each factory preset carries floats (cutoffHz, ampDecayMs, modDepth, ...) and, separately, a 0..127
// default per knob that is pre-seeded onto the MPK Mini MK4 encoders (SysEx 0x12). The first knob turn
// sends a value next to that default, so a default that does not reproduce the loaded float makes the
// sound jump. This test fails if any knob's default lands more than one CC step away from the value the
// preset loads.
//
//   knob_defaults_test          assert every knob default matches (CTest: KnobDefaultsTest)
//   knob_defaults_test --list   probe: print every mismatching knob with the CC that would match
//                               (X = mismatch, ~ = allowed mismatch, see isAllowedMismatch())
//   knob_defaults_test --all    probe: print every knob

#include "SynthEngine.h"
#include "SysExProtocol.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Synth;
using PS = ParameterScaler;

namespace {

// Engine state a knob controls: `state` for on/off and enum parts, `value` for the continuous part
// (0 while the parameter is switched off, e.g. chorus mix with the chorus disabled).
struct KnobReading {
    int   state{0};
    float value{0.0f};
};

// Knobs whose value is engine-global rather than part of the preset (never reset by a preset load)
bool isEngineGlobal(uint8_t cc) {
    return cc == CC_VOLUME;
}

// Mismatches no default can fix. TB-303 Slide: >= 64 switches slide on and already means 101 ms of glide,
// longer than the preset's 60 ms, so 64 (the closest default that keeps slide on) stays.
bool isAllowedMismatch(SynthModel model, uint8_t cc) {
    return model == SynthModel::TB303 && cc == CC_PORTAMENTO_ENABLE;
}

// Mirrors SynthEngine::controlChange(): what each CC sets in the current engine mode / shift state
KnobReading readKnob(const SynthEngine& e, uint8_t cc) {
    const bool fm    = e.getEngineMode() == EngineMode::FM;
    const bool casio = isCasioModel(e.getCurrentPreset().model);
    const bool shift = e.isShiftActive();
    switch (cc) {
        case CC_FILTER_CUTOFF:      return {0, fm ? e.getModDepth() : e.getCutoffHz()};
        case CC_FILTER_RESONANCE:   return {0, fm ? e.getModRatio() : e.getResonanceQ()};
        case CC_OSC_WAVEFORM:
            if (fm) return {0, e.getModDepth()};
            return {casio ? static_cast<int>(e.getCasioTone()) : static_cast<int>(e.getWaveform()), 0.0f};
        case CC_AMP_ATTACK:         return {0, e.getAmpAttackMs()};
        case CC_AMP_DECAY:          return {0, e.getAmpDecayMs()};
        case CC_AMP_SUSTAIN:        return {0, e.getAmpSustainLevel()};
        case CC_AMP_RELEASE:        return {0, e.getAmpReleaseMs()};
        case CC_FILTER_ENV_AMT:
            if (fm) return {0, e.getFilterDecayMs()};
            return {0, (casio && shift) ? e.getPulseWidth() : e.getFilterEnvAmount()};
        case CC_SUB_OSC_LEVEL:      return {0, fm ? e.getModFeedback() : e.getSubOscLevel()};
        case CC_CHORUS_MIX:
        case CC_CHORUS_SEND:        return {e.isChorusEnabled(), e.isChorusEnabled() ? e.getChorusMix() : 0.0f};
        case CC_DRIVE:              return {0, e.getDrive()};
        case CC_ACCENT:             return {0, e.getAccentAmount()};
        case CC_LFO_RATE:           return {0, e.getVibratoRateHz()};
        case CC_LFO_DEPTH:          return {0, e.getVibratoDepth()};
        case CC_VOICE_MODE:         return {static_cast<int>(e.getVoiceMode()), 0.0f};
        case CC_GLIDE:              return {e.isPortamentoEnabled(), e.isPortamentoEnabled() ? e.getGlideTimeMs() : 0.0f};
        case CC_DETUNE_DEPTH:       return {0, e.getDetuneCents()};
        case CC_FILTER_ENV_ATTACK:  return {0, fm ? e.getFilterSustainLevel() : e.getFilterAttackMs()};
        case CC_FILTER_ENV_DECAY:   return {0, fm ? e.getFilterAttackMs() : e.getFilterDecayMs()};
        case CC_FILTER_ENV_SUSTAIN:
            if (fm) return {0, e.getTremoloDepth()};
            return {0, (casio && shift) ? e.getSubOscLevel() : e.getFilterSustainLevel()};
        case CC_FILTER_ENV_RELEASE: return {0, fm ? e.getTremoloRateHz() : e.getFilterReleaseMs()};
        case CC_CHORUS_RATE:    return {0, e.getChorusRateHz()};
        case CC_CHORUS_DEPTH:   return {0, e.getChorusDepthMs()};
        case CC_PORTAMENTO_ENABLE: {
            const bool tb303 = e.getCurrentPreset().model == SynthModel::TB303;
            return {e.isPortamentoEnabled(), (tb303 && e.isPortamentoEnabled()) ? e.getGlideTimeMs() : 0.0f};
        }
        case CC_VOLUME:             return {0, e.getMasterVolume()};
        case CC_WT_POSITION:        return {0, e.getTimbre().wtPosition};
        case CC_WT_ENV_AMT:         return {0, e.getTimbre().wtEnvAmount};
        case CC_WT_TABLE:           return {e.getTimbre().wtTable, 0.0f};
        case CC_PD_DCW:             return {0, e.getTimbre().pdDcw};
        case CC_PD_WAVE:            return {static_cast<int>(e.getTimbre().pdWave), 0.0f};
        case CC_PLUCK_DECAY:        return {0, e.getTimbre().pluckDecayMs};
        case CC_PLUCK_BRIGHT:       return {0, e.getTimbre().pluckBright};
        case CC_PLUCK_PICK:         return {0, e.getTimbre().pluckPick};
        case CC_PERC_LEVEL:         return {0, e.getTimbre().percLevel};
        case CC_PERC_HARMONIC:      return {e.getTimbre().percHarmonic, 0.0f};
        case CC_KEY_CLICK:          return {0, e.getTimbre().keyClick};
        case CC_TREMOLO_DEPTH:      return {0, e.getTremoloDepth()};
        case CC_TREMOLO_RATE:       return {0, e.getTremoloRateHz()};
        case CC_OPX_ALGO:           return {static_cast<int>(e.getTimbre().opxAlgo), 0.0f};
        default:
            if (cc >= CC_DRAWBAR_1 && cc <= CC_DRAWBAR_9) {
                return {static_cast<int>(e.getTimbre().drawbars[cc - CC_DRAWBAR_1]), 0.0f};
            }
            std::fprintf(stderr, "readKnob: no mapping for CC %d; add one mirroring SynthEngine::controlChange()\n", cc);
            assert(false);
            return {};
    }
}

// Patches stored in user slot U01 (preset index FACTORY_PRESET_COUNT) over PatchDumpV2, as the patch sender does
Preset cosmosPatch() { // tools/patch_sender.html COSMOS: slow CS-80 pad, values far from the user knob template
    Preset p = getPresets()[FACTORY_PRESET_COUNT];
    p.name = "COSMOS"; p.voicing = SynthModel::CS80; p.chorusEnabled = true;
    p.subOscLevel = 0.3f; p.detuneCents = 18.0f; p.drive = 1.15f;
    p.cutoffHz = 900.0f; p.resonanceQ = 6.0f; p.filterEnvAmount = 0.35f;
    p.ampAttackMs = 1800.0f; p.ampDecayMs = 2500.0f; p.ampSustain = 0.85f; p.ampReleaseMs = 4500.0f;
    p.filterAttackMs = 2500.0f; p.filterDecayMs = 4000.0f; p.filterSustain = 0.4f; p.filterReleaseMs = 4000.0f;
    p.chorusRateHz = 0.25f; p.chorusDepthMs = 4.0f; p.chorusMix = 0.7f;
    return p;
}

Preset fmUserPatch() {
    Preset p = getPresets()[FACTORY_PRESET_COUNT];
    p.name = "FM BELL"; p.engineMode = EngineMode::FM;
    p.modRatio = 3.5f; p.modDepth = 7.0f; p.modFeedback = 0.4f; p.tremoloDepth = 0.2f; p.tremoloRateHz = 6.0f;
    p.ampAttackMs = 3.0f; p.ampDecayMs = 1200.0f; p.ampSustain = 0.2f; p.ampReleaseMs = 900.0f;
    p.filterAttackMs = 40.0f; p.filterDecayMs = 600.0f; p.filterSustain = 0.1f;
    return p;
}

std::unique_ptr<SynthEngine> loadFresh(size_t preset, bool bankB, const Preset* userPatch) {
    auto e = std::make_unique<SynthEngine>();
    e->init(44100.0f);
    if (userPatch) {
        uint8_t buf[MAX_SYSEX_MESSAGE_SIZE];
        std::vector<uint8_t> reply;
        e->handleSysEx(buf, SysExProtocol::encodePatchDumpV2(16, *userPatch, buf, sizeof(buf)), reply);
    }
    e->loadPresetIndex(preset);
    e->setShiftActive(bankB);
    return e;
}

KnobReading readAfterCC(size_t preset, bool bankB, const Preset* userPatch, uint8_t cc, int value) {
    auto e = loadFresh(preset, bankB, userPatch);
    e->controlChange(cc, static_cast<uint8_t>(std::clamp(value, 0, 127)));
    return readKnob(*e, cc);
}

struct KnobCheck {
    std::string loadedText, knobText; // describeParameter() right after loading, and after controlChange(default)
    KnobReading loaded, atDefault;
    uint8_t     ideal{0};
    bool        mismatch{false};
};

// The default matches when it selects the loaded on/off/enum state and the loaded continuous value lies
// within one CC step of it (between what default-1 and default+1 produce): a correctly rounded inverse
// always passes, whatever the scaling curve.
KnobCheck checkKnob(size_t preset, bool bankB, const Preset* userPatch, size_t k) {
    auto e = loadFresh(preset, bankB, userPatch);
    const auto& bank  = bankB ? e->getCurrentPreset().bankB : e->getCurrentPreset().bankA;
    const uint8_t cc  = bank.ccs[k];
    const uint8_t def = bank.defaults[k];

    KnobCheck c;
    std::string label;
    e->describeParameter(cc, def, label, c.loadedText);
    c.loaded = readKnob(*e, cc);
    c.ideal  = e->knobValueFor(cc, bankB, def);

    e->controlChange(cc, def);
    e->describeParameter(cc, def, label, c.knobText);
    c.atDefault = readKnob(*e, cc);

    // A neighbour that flips the on/off state (e.g. Slide at 63) says nothing about the value range
    auto neighbour = [&](int v) {
        const KnobReading r = readAfterCC(preset, bankB, userPatch, cc, v);
        return r.state == c.atDefault.state ? r.value : c.atDefault.value;
    };
    const float below = neighbour(def - 1);
    const float above = neighbour(def + 1);
    const float lo  = std::min({below, c.atDefault.value, above});
    const float hi  = std::max({below, c.atDefault.value, above});
    const float eps = 1e-4f * std::max(1.0f, std::fabs(c.loaded.value));
    c.mismatch = c.atDefault.state != c.loaded.state || c.loaded.value < lo - eps || c.loaded.value > hi + eps;
    return c;
}

} // namespace

int main(int argc, char** argv) {
    const bool listAll  = argc > 1 && std::strcmp(argv[1], "--all") == 0;
    const bool listMism = listAll || (argc > 1 && std::strcmp(argv[1], "--list") == 0);
    if (!listMism) std::printf("--- Running Knob Default Drift Tests ---\n");

    // Factory presets, then user slot U01: blank, and holding patches whose values are far from the knob template
    struct Case { size_t index; Preset user; bool hasUser; };
    std::vector<Case> cases;
    for (size_t pi = 0; pi <= FACTORY_PRESET_COUNT; ++pi) cases.push_back({pi, {}, false});
    cases.push_back({FACTORY_PRESET_COUNT, cosmosPatch(), true});
    cases.push_back({FACTORY_PRESET_COUNT, fmUserPatch(), true});

    int mismatches = 0;
    for (const Case& cs : cases) {
        const Preset* user = cs.hasUser ? &cs.user : nullptr;
        auto loaded = loadFresh(cs.index, false, user);
        const Preset& p = loaded->getCurrentPreset();
        for (int b = 0; b < 2; ++b) {
            for (size_t k = 0; k < 8; ++k) {
                const auto& bank = b ? p.bankB : p.bankA;
                if (isEngineGlobal(bank.ccs[k])) continue;

                const KnobCheck c = checkKnob(cs.index, b != 0, user, k);
                const bool allowed = c.mismatch && isAllowedMismatch(p.model, bank.ccs[k]);
                if (c.mismatch && !allowed) ++mismatches;
                if (c.mismatch || listAll) {
                    std::printf("%-2s %-12s K%-2zu %-8s cc%-3d default %3d -> ideal %3d   loaded %-12s knob %-12s\n",
                                allowed ? "~" : (c.mismatch ? "X" : ""), p.name, b * 8 + k + 1, bank.labels[k], bank.ccs[k],
                                bank.defaults[k], c.ideal, c.loadedText.c_str(), c.knobText.c_str());
                }
            }
        }
    }

    if (listMism) {
        std::printf("%d knob default(s) differ from the loaded preset value\n", mismatches);
        return 0;
    }
    if (mismatches) {
        std::fprintf(stderr, "%d knob default(s) differ from the loaded preset value; run knob_defaults_test --list\n",
                     mismatches);
    }
    assert(mismatches == 0);
    std::printf("All knob defaults match their preset values.\n");
    return 0;
}
