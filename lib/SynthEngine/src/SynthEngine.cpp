#include "SynthEngine.h"
#include "Wavetables.h"
#include "SysExProtocol.h"
#include "SineLut.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(ARDUINO) || defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#elif defined(MOCK_HAL)
#include "Arduino.h"
#else
#if __has_include(<Arduino.h>)
#include <Arduino.h>
#elif __has_include("Arduino.h")
#include "Arduino.h"
#else
#include <chrono>
static inline uint32_t millis() {
    static const auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}
#endif
#endif

namespace Synth {

// Voices are summed linearly (like an analog mixer bus). This trim leaves headroom for chords so the
// output limiter stays in its linear region for normal playing and only catches peaks.
static constexpr float kMasterBusTrim = 0.40f;
// The metronome click shares the master bus (and its chord-headroom trim), so it gets its own
// make-up gain: at the default level a downbeat peaks near full scale instead of buried in the mix.
static constexpr float kMetroClickGain = 4.4f;
// Sidechain duck: while a click rings, the music is pulled down by up to this fraction so the click
// stays audible over dense chords that already sit in the output limiter.
static constexpr float kMetroDuckDepth = 0.5f;

SynthEngine::SynthEngine() {
    heldKeys_.reserve(128); // max unique MIDI notes: no reallocation on the audio thread
    init(SynthConfig::DEFAULT_SAMPLE_RATE);
}

void SynthEngine::init(float sampleRate) {
    sampleRate_ = sampleRate;
    SineLut::init();
    for (auto& voice : voices_) {
        voice.init(sampleRate_);
    }
    chorus_.init(sampleRate_);

    // Initialize all User Preset Slots (#24..#32) with default factory User Presets
    const auto& presets = getPresets();
    for (size_t i = 0; i < NUM_USER_PRESETS; ++i) {
        userPresets_[i] = presets[FACTORY_PRESET_COUNT + i];
        std::strncpy(userPresetNames_[i].data(), userPresets_[i].name, userPresetNames_[i].size() - 1);
        userPresetNames_[i][userPresetNames_[i].size() - 1] = '\0';
        userPresets_[i].name = userPresetNames_[i].data();
        userStorageMemory_[i].clear();
    }

    // Attempt to load all user patches from storage
    loadAllUserPresetsFromStorage();

    loadPreset(SynthModel::Juno60);
}

void SynthEngine::setSampleRate(float sampleRate) {
    sampleRate_ = sampleRate;
    updateVoices([&](Voice& v) { v.setSampleRate(sampleRate_); });
    chorus_.setSampleRate(sampleRate_);
    updateAllVoices();
}

const Preset& SynthEngine::getCurrentPreset() const {
    const auto& presets = getPresets();
    if (isUserModel(static_cast<SynthModel>(currentPresetIndex_))) {
        size_t userIdx = currentPresetIndex_ - FACTORY_PRESET_COUNT;
        return userPresets_[userIdx];
    }
    return presets[currentPresetIndex_];
}

const KnobBank& SynthEngine::getActiveKnobBank() const {
    const auto& p = getCurrentPreset();
    return shiftActive_ ? p.bankB : p.bankA;
}

void SynthEngine::setMpkDisplaySendCallbacks(MpkMiniMk4::DisplayDriver::SendPacketFn pluginPortSend,
                                             MpkMiniMk4::DisplayDriver::SendPacketFn swControlPortSend) {
    mpkDisplay_.setPluginPortSender(std::move(pluginPortSend));
    mpkDisplay_.setSoftwareControlPortSender(std::move(swControlPortSend));
    updateMpkMiniDisplay();
}

void SynthEngine::updateMpkMiniDisplay(const char* line1Override, const char* line2Override) {
    if (!mpkDisplay_.hasPluginPortSender() && !mpkDisplay_.hasSoftwareControlPortSender()) {
        return;
    }

    const auto& p    = getCurrentPreset();
    const auto& bank = shiftActive_ ? p.bankB : p.bankA;
    std::string progHeader = std::string(p.name ? p.name : "SYNTH") + (shiftActive_ ? " [B]" : " [A]");
    auto theme = MpkMiniMk4::DisplayDriver::themeForPresetName(p.name ? p.name : "SYNTH", shiftActive_);

    // 1. Sync 284-byte Active RAM preset (Slot 0) on Software Control Port ONLY if Plugin Port is not connected!
    // (When Plugin Port is active, writing 284-byte RAM preset resets the hardware out of Plugin Mode and resets knob accumulators)
    if (mpkDisplay_.hasSoftwareControlPortSender() && !mpkDisplay_.hasPluginPortSender()) {
        std::array<std::string, 8> knobLabels;
        for (size_t k = 0; k < 8; ++k) {
            knobLabels[k] = bank.labels[k] ? bank.labels[k] : "";
        }
        mpkDisplay_.syncRamPreset(progHeader, knobLabels, theme.padColor);
    }

    // 2. Write 3-line formatted screen on Plugin Port (Line 0 Centered Header, Line 1 Left, Line 2 Right)
    if (mpkDisplay_.hasPluginPortSender()) {
        std::string l1 = line1Override ? line1Override : (shiftActive_ ? "BANK B: K9-K16" : "BANK A: K1-K8");
        std::string l2 = line2Override ? line2Override
                                       : (std::string(bank.labels[0] ? bank.labels[0] : "Cut") + " / " +
                                          std::string(bank.labels[1] ? bank.labels[1] : "Res") + " >");

        MpkMiniMk4::LineSpec s0(progHeader, MpkMiniMk4::Align::Center, theme.headerFg,
                                theme.headerBg, MpkMiniMk4::Layout::ThreeLineHeader);
        MpkMiniMk4::LineSpec s1(l1, MpkMiniMk4::Align::Center, theme.mutedFg,
                                theme.bodyBg, MpkMiniMk4::Layout::ThreeLineNormal);
        MpkMiniMk4::LineSpec s2(l2, MpkMiniMk4::Align::Center, theme.accent,
                                theme.bodyBg, MpkMiniMk4::Layout::ThreeLineNormal);
        mpkDisplay_.writeScreen3Lines(s0, s1, s2, true);

        // 3. Pre-seed hardware rotary encoder accumulators (knobState[0..7].currentValue @ 0x2000fee0) via SysEx 0x12
        std::array<uint8_t, 8> knobVals;
        for (size_t k = 0; k < 8; ++k) {
            knobVals[k] = bank.defaults[k];
        }
        mpkDisplay_.syncKnobValues(knobVals);
    }
}


// Preset knob labels are abbreviated to fit the MPK's 8-char knob names; expand them for the OLED HUD
// (kept <= 14 chars so the 160 px line never triggers the firmware marquee scroll).
static const char* expandKnobLabel(const char* label) {
    static const struct { const char* abbr; const char* full; } kNames[] = {
        {"Cutoff", "Filter Cutoff"},  {"Resonnc", "Resonance"},     {"SubOsc", "Sub Oscillator"},
        {"Chorus", "Chorus Mix"},     {"Attack", "Amp Attack"},     {"Decay", "Amp Decay"},
        {"Sustain", "Amp Sustain"},   {"Release", "Amp Release"},   {"FiltEnv", "Filter Env Amt"},
        {"F-Attack", "Filter Attack"},{"F-Decay", "Filter Decay"},  {"F-Sust", "Filter Sustain"},
        {"F-Rel", "Filter Release"},  {"ChoRate", "Chorus Rate"},   {"ChoDepth", "Chorus Depth"},
        {"Master", "Master Volume"},  {"Detune", "Osc Detune"},     {"Wavefrm", "Waveform"},
        {"VibRate", "Vibrato Rate"},  {"VibDepth", "Vibrato Depth"},{"TremRate", "Tremolo Rate"},
        {"ModDepth", "Mod Depth"},    {"ModAtt", "Mod Attack"},
        {"ModDecay", "Mod Decay"},    {"ModSust", "Mod Sustain"},   {"Ratio", "FM Ratio"},
        {"Feedback", "FM Feedback"},  {"PulseWid", "Pulse Width"},  {"PolyMode", "Poly Mode"},
        {"PolyMod", "Poly Mod"},      {"Distort", "Distortion"},  {"ModRate", "LFO Rate"},
        {"WavePos", "Wave Position"}, {"WaveEnv", "Wave Env Amt"},  {"Table", "Wavetable"},
        {"DCW", "DCW Amount"},        {"DCW-Env", "DCW Env Amt"},   {"Wave", "CZ Waveform"},
        {"DCW-Att", "DCW Attack"},    {"DCW-Dec", "DCW Decay"},     {"DCW-Sus", "DCW Sustain"},
        {"DCW-Rel", "DCW Release"},   {"StrDecay", "String Decay"}, {"Bright", "Brightness"},
        {"PickPos", "Pick Position"}, {"Perc", "Percussion"},       {"PercHarm", "Perc Harmonic"},
        {"PercDec", "Perc Decay"},    {"Click", "Key Click"},       {"Leslie", "Leslie Depth"},
        {"LeslRate", "Leslie Rate"},  {"16'", "Drawbar 16'"},       {"5 1/3'", "Drawbar 5 1/3'"},
        {"8'", "Drawbar 8'"},         {"4'", "Drawbar 4'"},         {"2 2/3'", "Drawbar 2 2/3'"},
        {"2'", "Drawbar 2'"},         {"1 3/5'", "Drawbar 1 3/5'"}, {"1 1/3'", "Drawbar 1 1/3'"},
        {"1'", "Drawbar 1'"},
    };
    for (const auto& n : kNames) {
        if (std::strcmp(label, n.abbr) == 0) return n.full;
    }
    return label; // Drive, Tone, Glide, Octave, Slide, Accent, Tremolo are already full words
}

bool SynthEngine::describeParameter(uint8_t ccNumber, uint8_t value, std::string& outLabel, std::string& outValue) {
    const auto& p    = getCurrentPreset();
    const auto& bank = shiftActive_ ? p.bankB : p.bankA;

    // Resolve label from active knob bank if it matches one of the 8 bank CCs
    std::string label;
    for (size_t k = 0; k < 8; ++k) {
        if (bank.ccs[k] == ccNumber && bank.labels[k]) {
            label = expandKnobLabel(bank.labels[k]);
            break;
        }
    }

    char valBuf[32] = {0};
    switch (ccNumber) {
        case CC_FILTER_CUTOFF:
            if (engineMode_ == EngineMode::FM) {
                if (label.empty()) label = "MOD DEPTH";
                std::snprintf(valBuf, sizeof(valBuf), "%.2f", static_cast<double>(modDepth_));
                break;
            }
            if (label.empty()) label = "FILTER CUTOFF";
            std::snprintf(valBuf, sizeof(valBuf), "%d Hz", static_cast<int>(cutoffHz_));
            break;
        case CC_FILTER_RESONANCE:
            if (engineMode_ == EngineMode::FM) {
                if (label.empty()) label = "FM RATIO";
                std::snprintf(valBuf, sizeof(valBuf), "%.2f : 1", static_cast<double>(modRatio_));
                break;
            }
            if (label.empty()) label = "RESONANCE";
            std::snprintf(valBuf, sizeof(valBuf), "%.1f Q", static_cast<double>(resonanceQ_));
            break;
        case CC_OSC_WAVEFORM:
            if (label.empty()) label = "WAVEFORM";
            if (isCasioModel(p.model)) {
                std::snprintf(valBuf, sizeof(valBuf), "%s", getCasioToneName());
            } else {
                const char* w = (waveform_ == Waveform::Sawtooth) ? "SAW" :
                                (waveform_ == Waveform::Square ? "SQUARE" : "TRIANGLE");
                std::snprintf(valBuf, sizeof(valBuf), "%s", w);
            }
            break;
        case CC_AMP_ATTACK:
            if (label.empty()) label = "AMP ATTACK";
            std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(ampAttackMs_));
            break;
        case CC_AMP_DECAY:
            if (label.empty()) label = "AMP DECAY";
            std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(ampDecayMs_));
            break;
        case CC_AMP_SUSTAIN:
            if (label.empty()) label = "AMP SUSTAIN";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(ampSustainLevel_ * 100.0f));
            break;
        case CC_AMP_RELEASE:
            if (label.empty()) label = "AMP RELEASE";
            std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(ampReleaseMs_));
            break;
        case CC_FILTER_ENV_AMT: // mirrors controlChange(): FM Mod Decay / Casio Pulse Width (shifted) / Filter Env Amount
            if (engineMode_ == EngineMode::FM) {
                if (label.empty()) label = "MOD DECAY";
                std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(filterDecayMs_));
            } else if (isCasioModel(p.model) && shiftActive_) {
                if (label.empty()) label = "PULSE WIDTH";
                std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(pulseWidth_ * 100.0f));
            } else {
                if (label.empty()) label = "FLT ENV AMT";
                std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(filterEnvAmount_ * 100.0f));
            }
            break;
        case CC_FILTER_ENV_ATTACK: // FM Mod Sustain / Filter Attack
            if (engineMode_ == EngineMode::FM) {
                if (label.empty()) label = "MOD SUSTAIN";
                std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(filterSustainLevel_ * 100.0f));
            } else {
                if (label.empty()) label = "FILTER ATTACK";
                std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(filterAttackMs_));
            }
            break;
        case CC_FILTER_ENV_DECAY: // FM Mod Attack / Filter Decay
            if (engineMode_ == EngineMode::FM) {
                if (label.empty()) label = "MOD ATTACK";
                std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(filterAttackMs_));
            } else {
                if (label.empty()) label = "FILTER DECAY";
                std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(filterDecayMs_));
            }
            break;
        case CC_FILTER_ENV_SUSTAIN: // FM Tremolo Depth / Casio Sub Osc (shifted) / Filter Sustain
            if (engineMode_ == EngineMode::FM) {
                if (label.empty()) label = "TREMOLO";
                std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(tremoloDepth_ * 100.0f));
            } else if (isCasioModel(p.model) && shiftActive_) {
                if (label.empty()) label = "SUB OSC";
                std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(subOscLevel_ * 100.0f));
            } else {
                if (label.empty()) label = "FILTER SUSTAIN";
                std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(filterSustainLevel_ * 100.0f));
            }
            break;
        case CC_FILTER_ENV_RELEASE: // FM Tremolo Rate / Filter Release
            if (engineMode_ == EngineMode::FM) {
                if (label.empty()) label = "TREMOLO RATE";
                std::snprintf(valBuf, sizeof(valBuf), "%.1f Hz", static_cast<double>(tremoloRateHz_));
            } else {
                if (label.empty()) label = "FILTER RELEASE";
                std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(filterReleaseMs_));
            }
            break;
        case CC_CHORUS_RATE:
            if (label.empty()) label = "CHORUS RATE";
            std::snprintf(valBuf, sizeof(valBuf), "%.1f Hz", static_cast<double>(chorus_.getRateHz()));
            break;
        case CC_CHORUS_DEPTH:
            if (label.empty()) label = "CHORUS DEPTH";
            std::snprintf(valBuf, sizeof(valBuf), "%.1f ms", static_cast<double>(chorus_.getDepthMs()));
            break;
        case CC_SUB_OSC_LEVEL:
            if (label.empty()) label = "SUB OSC";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>((value * 100) / 127));
            break;
        case CC_CHORUS_MIX:
        case CC_CHORUS_SEND:
            if (label.empty()) label = "CHORUS MIX";
            if (!chorus_.isEnabled()) std::snprintf(valBuf, sizeof(valBuf), "OFF"); // CC 0..10 bypasses the chorus
            else std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(chorus_.getMix() * 100.0f));
            break;
        case CC_VOLUME:
        case CC_FM_MASTER_VOL:
            if (label.empty()) label = "MASTER VOL";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(masterGain_ * 100.0f));
            break;
        case CC_DRIVE:
            if (label.empty()) label = "DRIVE";
            std::snprintf(valBuf, sizeof(valBuf), "%.2fx", static_cast<double>(drive_));
            break;
        case CC_DETUNE_DEPTH:
            if (label.empty()) label = "OSC DETUNE";
            std::snprintf(valBuf, sizeof(valBuf), "%.1f ct", static_cast<double>(detuneCents_));
            break;
        case CC_ACCENT:
            if (label.empty()) label = "ACCENT";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(accentAmount_ * 100.0f));
            break;
        case CC_LFO_RATE:
            if (label.empty()) label = "LFO RATE";
            std::snprintf(valBuf, sizeof(valBuf), "%.1f Hz", static_cast<double>(lfoRateHz_));
            break;
        case CC_LFO_DEPTH:
            if (label.empty()) label = "LFO DEPTH";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(vibratoDepth_ * 100.0f));
            break;
        case CC_VOICE_MODE:
            if (label.empty()) label = "VOICE MODE";
            std::snprintf(valBuf, sizeof(valBuf), "%s", voiceMode_ == VoiceMode::Monophonic ? "MONO" : "POLY");
            break;
        case CC_GLIDE:
            if (label.empty()) label = "GLIDE";
            if (portamentoEnabled_) std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(glideTimeMs_));
            else std::snprintf(valBuf, sizeof(valBuf), "OFF");
            break;
        case CC_PORTAMENTO_ENABLE: // mirrors controlChange(): switch at 64; TB-303 also maps value to glide time
            if (label.empty()) label = "SLIDE";
            if (!portamentoEnabled_) std::snprintf(valBuf, sizeof(valBuf), "OFF");
            else if (p.model == SynthModel::TB303) std::snprintf(valBuf, sizeof(valBuf), "%d ms", static_cast<int>(glideTimeMs_));
            else std::snprintf(valBuf, sizeof(valBuf), "ON");
            break;
        case CC_WT_POSITION:
            if (label.empty()) label = "WAVE POSITION";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(timbre_.wtPosition * 100.0f));
            break;
        case CC_WT_ENV_AMT:
            if (label.empty()) label = "WAVE ENV AMT";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(timbre_.wtEnvAmount * 100.0f));
            break;
        case CC_WT_TABLE:
            if (label.empty()) label = "WAVETABLE";
            std::snprintf(valBuf, sizeof(valBuf), "%s", Wavetables::tableName(timbre_.wtTable));
            break;
        case CC_PD_DCW:
            if (label.empty()) label = "DCW AMOUNT";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(timbre_.pdDcw * 100.0f));
            break;
        case CC_PD_WAVE: {
            static const char* const kWaves[] = {"SAW", "SQUARE", "PULSE", "DBL SINE", "RESO SAW", "RESO TRI", "RESO TRAP"};
            if (label.empty()) label = "CZ WAVEFORM";
            std::snprintf(valBuf, sizeof(valBuf), "%s", kWaves[static_cast<uint8_t>(timbre_.pdWave) % 7]);
            break;
        }
        case CC_PLUCK_DECAY:
            if (label.empty()) label = "STRING DECAY";
            std::snprintf(valBuf, sizeof(valBuf), "%.1f s", static_cast<double>(timbre_.pluckDecayMs * 0.001f));
            break;
        case CC_PLUCK_BRIGHT:
            if (label.empty()) label = "BRIGHTNESS";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(timbre_.pluckBright * 100.0f));
            break;
        case CC_PLUCK_PICK:
            if (label.empty()) label = "PICK POSITION";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(timbre_.pluckPick * 100.0f));
            break;
        case CC_PERC_LEVEL:
            if (label.empty()) label = "PERCUSSION";
            if (timbre_.percLevel <= 0.0f) std::snprintf(valBuf, sizeof(valBuf), "OFF");
            else std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(timbre_.percLevel * 100.0f));
            break;
        case CC_PERC_HARMONIC:
            if (label.empty()) label = "PERC HARMONIC";
            std::snprintf(valBuf, sizeof(valBuf), "%s", timbre_.percHarmonic ? "THIRD" : "SECOND");
            break;
        case CC_KEY_CLICK:
            if (label.empty()) label = "KEY CLICK";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(timbre_.keyClick * 100.0f));
            break;
        case CC_TREMOLO_DEPTH:
            if (label.empty()) label = "TREMOLO";
            std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(tremoloDepth_ * 100.0f));
            break;
        case CC_TREMOLO_RATE:
            if (label.empty()) label = "TREMOLO RATE";
            std::snprintf(valBuf, sizeof(valBuf), "%.1f Hz", static_cast<double>(tremoloRateHz_));
            break;
        case CC_DRAWBAR_1:
        case CC_DRAWBAR_1 + 1:
        case CC_DRAWBAR_1 + 2:
        case CC_DRAWBAR_1 + 3:
        case CC_DRAWBAR_1 + 4:
        case CC_DRAWBAR_1 + 5:
        case CC_DRAWBAR_1 + 6:
        case CC_DRAWBAR_1 + 7:
        case CC_DRAWBAR_9: {
            static const char* const kFeet[DRAWBAR_COUNT] = {"16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"};
            const size_t bar = ccNumber - CC_DRAWBAR_1;
            if (label.empty()) label = std::string("DRAWBAR ") + kFeet[bar];
            std::snprintf(valBuf, sizeof(valBuf), "%d", static_cast<int>(std::lround(timbre_.drawbars[bar])));
            break;
        }
        case CC_TEMPO_BPM:
            if (label.empty()) label = "TEMPO BPM";
            std::snprintf(valBuf, sizeof(valBuf), "%d BPM", static_cast<int>(metronomeBpm_));
            break;
        case CC_TIME_SIGNATURE:
        case CC_TIME_SIG_BEATS:
            if (label.empty()) label = (ccNumber == CC_TIME_SIGNATURE) ? "TIME SIG" : "BEATS/BAR";
            std::snprintf(valBuf, sizeof(valBuf), "%u/%u", static_cast<unsigned>(timeSigNumerator_),
                          static_cast<unsigned>(timeSigDenominator_));
            break;
        case CC_METRO_VOLUME:
            if (label.empty()) label = "METRO LEVEL";
            if (!metronomeEnabled_) std::snprintf(valBuf, sizeof(valBuf), "OFF");
            else std::snprintf(valBuf, sizeof(valBuf), "%d %%", static_cast<int>(metronomeVolume_ * 100.0f + 0.5f));
            break;
        case CC_METRO_KEY_TUNE:
            if (label.empty()) label = "KEY CLICK";
            std::snprintf(valBuf, sizeof(valBuf), "%s", metronomeKeyTuned_ ? "KEY" : "WOOD");
            break;
        case CC_METRO_CLICK:
            if (label.empty()) label = "CLICK";
            std::snprintf(valBuf, sizeof(valBuf), "%s", metronomeKeyTuned_ ? getRootKeyName() : "WOOD");
            break;
        case CC_METRO_SILENT:
            if (label.empty()) label = "METRO OUT";
            std::snprintf(valBuf, sizeof(valBuf), "%s", metronomeSilent_ ? "SILENT" : "SOUND");
            break;
        case CC_OUTPUT_MODE:
            if (label.empty()) label = "OUTPUT";
            std::snprintf(valBuf, sizeof(valBuf), "%s", monoOutput_ ? "MONO L" : "STEREO");
            break;
        case CC_ROOT_KEY:
            if (label.empty()) label = "ROOT KEY";
            std::snprintf(valBuf, sizeof(valBuf), "%s", getRootKeyName());
            break;
        case CC_SCALE_MODE:
            if (label.empty()) label = "SCALE MODE";
            std::snprintf(valBuf, sizeof(valBuf), "%s", getScaleModeName());
            break;
        default:
            if (label.empty()) {
                return false;
            }
            std::snprintf(valBuf, sizeof(valBuf), "%u", static_cast<unsigned>(value));
            break;
    }

    outLabel = label;
    outValue = valBuf;
    return true;
}

void SynthEngine::notifyMpkMiniParameterChanged(uint8_t ccNumber, uint8_t value) {
    if (!mpkDisplay_.hasPluginPortSender()) return;

    std::string label, valueText;
    if (!describeParameter(ccNumber, value, label, valueText)) return;

    const auto& p = getCurrentPreset();
    auto theme    = MpkMiniMk4::DisplayDriver::themeForPresetName(p.name ? p.name : "SYNTH", shiftActive_);
    mpkDisplay_.showKnobHud(p.name ? p.name : "SYNTH", label, valueText, value, theme);
}


void SynthEngine::setShiftActive(bool shift) {
    if (shiftActive_ != shift) {
        shiftActive_ = shift;
        updateMpkMiniDisplay();
        if (onDisplayUpdate_) {
            onDisplayUpdate_(getCurrentPreset(), shiftActive_);
        }
    }
}

void SynthEngine::toggleShift() {
    setShiftActive(!shiftActive_);
}

void SynthEngine::loadPreset(SynthModel model) {
    loadPresetIndex(static_cast<size_t>(model));
}

void SynthEngine::loadPresetIndex(size_t index) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    const auto& presets = getPresets();
    currentPresetIndex_ = index % presets.size();
    const auto& p = (isUserModel(static_cast<SynthModel>(currentPresetIndex_))) ?
                    userPresets_[currentPresetIndex_ - FACTORY_PRESET_COUNT] : presets[currentPresetIndex_];
    applyPreset(p);
}

void SynthEngine::snapshotActiveParameters(Preset& target) const {
    target.waveform          = waveform_;
    target.subOscLevel       = subOscLevel_;
    target.detuneCents       = detuneCents_;
    target.drive             = drive_;
    target.cutoffHz          = cutoffHz_;
    target.resonanceQ        = resonanceQ_;
    target.filterEnvAmount   = filterEnvAmount_;
    target.ampAttackMs       = ampAttackMs_;
    target.ampDecayMs        = ampDecayMs_;
    target.ampSustain        = ampSustainLevel_;
    target.ampReleaseMs      = ampReleaseMs_;
    target.filterAttackMs    = filterAttackMs_;
    target.filterDecayMs     = filterDecayMs_;
    target.filterSustain     = filterSustainLevel_;
    target.filterReleaseMs   = filterReleaseMs_;
    target.chorusEnabled     = chorus_.isEnabled();
    target.chorusRateHz      = chorus_.getRateHz();
    target.chorusDepthMs     = chorus_.getDepthMs();
    target.chorusMix         = chorus_.getMix();
    target.voiceMode         = voiceMode_;
    target.portamentoEnabled = portamentoEnabled_;
    target.legatoEnabled     = legatoEnabled_;
    target.glideTimeMs       = glideTimeMs_;
    target.engineMode        = engineMode_;
    target.modRatio          = modRatio_;
    target.modDepth          = modDepth_;
    target.modFeedback       = modFeedback_;
    target.tremoloDepth      = tremoloDepth_;
    target.tremoloRateHz     = tremoloRateHz_;
    target.voicing           = patchVoicing_;
    target.timbre            = timbre_;
}

static float ccToVibratoRateHz(uint8_t value) {
    return 0.5f + ParameterScaler::ccToNorm(value) * 14.5f;
}

// Default of the knob bound to `cc` in either bank, or nullptr if the preset has no such knob
static const uint8_t* findKnobDefault(const Preset& p, uint8_t cc) {
    for (const KnobBank* bank : {&p.bankA, &p.bankB}) {
        for (size_t k = 0; k < 8; ++k) {
            if (bank->ccs[k] == cc) return &bank->defaults[k];
        }
    }
    return nullptr;
}

static uint8_t linToCC(float v, float lo, float span) { return ParameterScaler::normToCC((v - lo) / span); }

// Mirrors the scalings in controlChange()
uint8_t SynthEngine::knobValueFor(uint8_t cc, bool shift, uint8_t current) const {
    using Scaler = ParameterScaler; // not PS: a register macro in the Xtensa headers
    const bool fm    = engineMode_ == EngineMode::FM;
    const bool casio = isCasioModel(getCurrentPreset().model);
    switch (cc) {
        case CC_FILTER_CUTOFF:
            return fm ? linToCC(modDepth_, 0.0f, 12.0f) : Scaler::cutoffHzToCC(cutoffHz_);
        case CC_FILTER_RESONANCE:
            return fm ? linToCC(modRatio_, 0.5f, 13.5f) : Scaler::resonanceQToCC(resonanceQ_);
        case CC_OSC_WAVEFORM: {
            if (fm) return linToCC(modDepth_, 0.0f, 12.0f);
            static const uint8_t toneCenters[] = {16, 48, 80, 112}; // < 32 / < 64 / < 96 / rest
            static const uint8_t waveCenters[] = {21, 64, 106};     // < 43 / < 86 / rest
            const uint8_t want = casio ? toneCenters[static_cast<int>(casioTone_)]
                                       : waveCenters[static_cast<int>(waveform_)];
            const bool sameBucket = casio ? (current / 32 == want / 32)
                                          : ((current < 43) == (want < 43) && (current < 86) == (want < 86));
            return sameBucket ? current : want;
        }
        case CC_AMP_ATTACK:  return Scaler::envelopeTimeMsToCC(ampAttackMs_, 1.0f, 4000.0f);
        case CC_AMP_DECAY:   return Scaler::envelopeTimeMsToCC(ampDecayMs_, 5.0f, 5000.0f);
        case CC_AMP_SUSTAIN: return Scaler::normToCC(ampSustainLevel_);
        case CC_AMP_RELEASE: return Scaler::envelopeTimeMsToCC(ampReleaseMs_, 5.0f, 5000.0f);
        case CC_FILTER_ENV_AMT:
            if (fm) return Scaler::envelopeTimeMsToCC(filterDecayMs_, 10.0f, 4000.0f);
            if (casio && shift) return linToCC(pulseWidth_, 0.05f, 0.90f);
            return Scaler::normToCC(filterEnvAmount_);
        case CC_SUB_OSC_LEVEL:
            return fm ? linToCC(modFeedback_, 0.0f, 1.5f) : Scaler::normToCC(subOscLevel_);
        case CC_CHORUS_MIX:
        case CC_CHORUS_SEND: { // values <= 10 switch the chorus off
            const uint8_t c = Scaler::normToCC(chorus_.getMix());
            return chorus_.isEnabled() ? std::max<uint8_t>(c, 11) : std::min<uint8_t>(c, 10);
        }
        case CC_DRIVE:       return linToCC(drive_, 1.0f, 2.5f);
        case CC_ACCENT:      return Scaler::normToCC(getAccentAmount());
        case CC_LFO_RATE:    return linToCC(getVibratoRateHz(), 0.5f, 14.5f);
        case CC_LFO_DEPTH:   return Scaler::normToCC(getVibratoDepth());
        case CC_VOICE_MODE: {
            const bool poly = voiceMode_ == VoiceMode::Polyphonic;
            return ((current >= 64) == poly) ? current : (poly ? 127 : 0);
        }
        case CC_GLIDE: // 0 = off
            return portamentoEnabled_ ? std::max<uint8_t>(1, linToCC(glideTimeMs_, 0.0f, 1000.0f)) : 0;
        case CC_DETUNE_DEPTH: return linToCC(detuneCents_, 0.0f, 50.0f);
        case CC_FILTER_ENV_ATTACK:
            return fm ? Scaler::normToCC(filterSustainLevel_) : Scaler::envelopeTimeMsToCC(filterAttackMs_, 1.0f, 4000.0f);
        case CC_FILTER_ENV_DECAY:
            return fm ? Scaler::envelopeTimeMsToCC(filterAttackMs_, 1.0f, 2000.0f)
                      : Scaler::envelopeTimeMsToCC(filterDecayMs_, 5.0f, 5000.0f);
        case CC_FILTER_ENV_SUSTAIN:
            if (fm) return Scaler::normToCC(tremoloDepth_);
            return Scaler::normToCC((casio && shift) ? subOscLevel_ : filterSustainLevel_);
        case CC_FILTER_ENV_RELEASE:
            return fm ? linToCC(tremoloRateHz_, 0.5f, 15.0f) : Scaler::envelopeTimeMsToCC(filterReleaseMs_, 5.0f, 5000.0f);
        case CC_CHORUS_RATE:  return linToCC(chorus_.getRateHz(), 0.1f, 4.9f);
        case CC_CHORUS_DEPTH: return linToCC(chorus_.getDepthMs(), 0.5f, 4.5f);
        case CC_PORTAMENTO_ENABLE: { // >= 64 on; TB-303 also maps the value to 0..200 ms of glide
            if (!portamentoEnabled_) return std::min<uint8_t>(current, 63);
            if (getCurrentPreset().model != SynthModel::TB303) return std::max<uint8_t>(current, 64);
            return std::max<uint8_t>(64, linToCC(glideTimeMs_, 0.0f, 200.0f));
        }
        case CC_VOLUME:      return Scaler::normToCC(masterGain_);
        case CC_WT_POSITION: return Scaler::normToCC(timbre_.wtPosition);
        case CC_WT_ENV_AMT:  return Scaler::normToCC(timbre_.wtEnvAmount);
        case CC_WT_TABLE: { // buckets of 128 / WAVETABLE_COUNT
            const int bucket = 128 / static_cast<int>(WAVETABLE_COUNT);
            return (current / bucket == timbre_.wtTable) ? current
                                                         : static_cast<uint8_t>(timbre_.wtTable * bucket + bucket / 2);
        }
        case CC_PD_DCW:      return Scaler::normToCC(timbre_.pdDcw);
        case CC_PD_WAVE: { // value * Count / 128
            const int w = static_cast<int>(timbre_.pdWave), n = static_cast<int>(PdWave::Count);
            if (current * n / 128 == w) return current;
            return static_cast<uint8_t>((w * 128 + 64) / n);
        }
        case CC_PLUCK_DECAY:  return Scaler::envelopeTimeMsToCC(timbre_.pluckDecayMs, 100.0f, 10000.0f);
        case CC_PLUCK_BRIGHT: return Scaler::normToCC(timbre_.pluckBright);
        case CC_PLUCK_PICK:   return Scaler::normToCC(timbre_.pluckPick);
        case CC_PERC_LEVEL:   return Scaler::normToCC(timbre_.percLevel);
        case CC_PERC_HARMONIC: {
            const bool third = timbre_.percHarmonic != 0;
            return ((current >= 64) == third) ? current : (third ? 127 : 0);
        }
        case CC_KEY_CLICK:     return Scaler::normToCC(timbre_.keyClick);
        case CC_TREMOLO_DEPTH: return Scaler::normToCC(tremoloDepth_);
        case CC_TREMOLO_RATE:  return linToCC(tremoloRateHz_, 0.5f, 15.0f);
        default:
            if (cc >= CC_DRAWBAR_1 && cc <= CC_DRAWBAR_9) { // round(cc / 127 * 8)
                const float bar = timbre_.drawbars[cc - CC_DRAWBAR_1];
                if (std::lround(current / 127.0f * 8.0f) == std::lround(bar)) return current;
                return Scaler::normToCC(bar / 8.0f);
            }
            return current;
    }
}

// User slots share one knob template whose defaults suit the blank user patch, not the stored one. Seed
// the encoders from the live sound instead, so the first knob turn continues from it rather than jumping.
// Master volume is engine-global and keeps its template default, as on the factory presets.
void SynthEngine::syncUserKnobDefaults() {
    if (!isUserModel(static_cast<SynthModel>(currentPresetIndex_))) return;
    Preset& p = userPresets_[currentPresetIndex_ - FACTORY_PRESET_COUNT];
    for (int b = 0; b < 2; ++b) {
        KnobBank& bank = b ? p.bankB : p.bankA;
        for (size_t k = 0; k < 8; ++k) {
            if (bank.ccs[k] == CC_VOLUME) continue;
            bank.defaults[k] = knobValueFor(bank.ccs[k], b != 0, bank.defaults[k]);
        }
    }
}

// Engine mode and instrument voicing. A generic patch (voicing Count) takes the default voicing of its
// engine, so switching it to FM and back restores the generic subtractive character.
void SynthEngine::setEngineModeAndVoicing(EngineMode mode, SynthModel patchVoicing) {
    if (mode != engineMode_) {
        // Tails of the old engine would be re-voiced by the new one mid-release: fade them instead
        if (deferredEvents_) pushEvent(EventType::FastReleaseAll);
        else fastReleaseAllImpl();
    }
    engineMode_    = mode;
    patchVoicing_  = patchVoicing;
    voicingModel_  = isFactoryModel(patchVoicing) ? patchVoicing : defaultVoicingForEngine(mode);
    voicing_       = getModelVoicing(voicingModel_);
    levelGain_     = std::pow(10.0f, getModelLevelDb(voicingModel_) / 20.0f);
    outputGain_    = masterGain_ * levelGain_;
}

void SynthEngine::applyPreset(const Preset& p) {
    waveform_        = p.waveform;
    subOscLevel_     = p.subOscLevel;
    detuneCents_     = p.detuneCents;
    drive_           = p.drive;
    cutoffHz_        = p.cutoffHz;
    resonanceQ_      = p.resonanceQ;
    filterEnvAmount_ = p.filterEnvAmount;

    ampAttackMs_     = p.ampAttackMs;
    ampDecayMs_      = p.ampDecayMs;
    ampSustainLevel_ = p.ampSustain;
    ampReleaseMs_    = p.ampReleaseMs;

    filterAttackMs_     = p.filterAttackMs;
    filterDecayMs_      = p.filterDecayMs;
    filterSustainLevel_ = p.filterSustain;
    filterReleaseMs_    = p.filterReleaseMs;

    chorus_.setEnabled(p.chorusEnabled);
    chorus_.setRateHz(p.chorusRateHz);
    chorus_.setDepthMs(p.chorusDepthMs);
    chorus_.setMix(p.chorusMix);

    setVoiceMode(p.voiceMode);
    setPortamentoEnabled(p.portamentoEnabled);
    setLegatoEnabled(p.legatoEnabled);
    setGlideTimeMs(p.glideTimeMs);

    setEngineModeAndVoicing(presetEngineMode(p), isFactoryModel(p.voicing) ? p.voicing
                                   : isFactoryModel(p.model)   ? p.model : SynthModel::Count);
    modRatio_       = p.modRatio;
    modDepth_       = p.modDepth;
    modFeedback_    = p.modFeedback;
    tremoloDepth_   = p.tremoloDepth;
    tremoloRateHz_  = p.tremoloRateHz;
    timbre_         = p.timbre;

    // Accent and vibrato rate have no preset field: the preset's knob default sets them, so they do not
    // carry over from the previous preset (the Casio tones below then set their own vibrato rate)
    const uint8_t* accentKnob = findKnobDefault(p, CC_ACCENT);
    setAccentAmount(accentKnob ? ParameterScaler::ccToNorm(*accentKnob) : DEFAULT_ACCENT_AMOUNT);
    const uint8_t* lfoRateKnob = findKnobDefault(p, CC_LFO_RATE);
    setVibratoRateHz(lfoRateKnob ? ccToVibratoRateHz(*lfoRateKnob) : DEFAULT_LFO_RATE_HZ);

    if (p.model == SynthModel::CasioPT10_Piano) {
        setCasioTone(CasioTone::Piano);
    } else if (p.model == SynthModel::CasioPT10_Fantasy) {
        setCasioTone(CasioTone::Fantasy);
    } else if (p.model == SynthModel::CasioPT10_Violin) {
        setCasioTone(CasioTone::Violin);
    } else if (p.model == SynthModel::CasioPT10_Flute) {
        setCasioTone(CasioTone::Flute);
    } else {
        pulseWidth_   = 0.50f;
        vibratoDepth_ = 0.0f;
    }

    syncUserKnobDefaults();
    updateAllVoices();
    updateMpkMiniDisplay();

    if (onDisplayUpdate_) {
        onDisplayUpdate_(p, shiftActive_);
    }
}

void SynthEngine::setVoiceMode(VoiceMode mode) {
    if (voiceMode_ != mode) {
        // Fade sounding voices (a hard kill clicks); their tails keep rendering until silent
        if (deferredEvents_) pushEvent(EventType::FastReleaseAll);
        else fastReleaseAllImpl();
        voiceMode_ = mode;
    }
}

void SynthEngine::setPortamentoEnabled(bool enabled) {
    portamentoEnabled_ = enabled;
    updateVoices([&](Voice& v) { v.setPortamentoEnabled(enabled); });
}

void SynthEngine::setGlideTimeMs(float ms) {
    glideTimeMs_ = std::clamp(ms, 0.0f, 2000.0f);
    updateVoices([&](Voice& v) { v.setGlideTimeMs(glideTimeMs_); });
}

const HeldKey* SynthEngine::resolveMonophonicNote() const {
    if (heldKeys_.empty()) return nullptr;

    switch (notePriority_) {
        case NotePriority::Last:
            return &heldKeys_.back();

        case NotePriority::Low: {
            const HeldKey* lowest = &heldKeys_[0];
            for (size_t i = 1; i < heldKeys_.size(); ++i) {
                if (heldKeys_[i].noteNumber < lowest->noteNumber) {
                    lowest = &heldKeys_[i];
                }
            }
            return lowest;
        }

        case NotePriority::High: {
            const HeldKey* highest = &heldKeys_[0];
            for (size_t i = 1; i < heldKeys_.size(); ++i) {
                if (heldKeys_[i].noteNumber > highest->noteNumber) {
                    highest = &heldKeys_[i];
                }
            }
            return highest;
        }
    }
    return &heldKeys_.back();
}

void SynthEngine::removeHeldKey(uint8_t rawNote) {
    heldKeys_.erase(
        std::remove_if(heldKeys_.begin(), heldKeys_.end(),
            [rawNote](const HeldKey& k) { return k.rawNote == rawNote; }),
        heldKeys_.end()
    );
}

void SynthEngine::noteOnMono(uint8_t noteNumber, uint8_t velocity, uint8_t rawNote) {
    // Track whether another key was already physically held before this note-on
    const bool hadHeldKeyBefore = !heldKeys_.empty();

    // Re-stack key at the end (Last Note Priority)
    removeHeldKey(rawNote);
    heldKeys_.push_back({noteNumber, velocity, rawNote});

    const HeldKey* target = resolveMonophonicNote();
    if (!target) return;

    Voice& v = voices_[0];
    // Only use single-trigger legato pitch change if:
    // 1. Legato mode is enabled
    // 2. Another key was still held down (overlapping legato)
    // 3. The voice is actively sustaining/decaying and NOT in EnvelopeStage::Release
    // 4. The patch has a non-zero sustain level (percussive 0-sustain patches like PT-10 Piano must retrigger)
    // Single-trigger legato (and TB-303 slide) whenever keys overlap on a sounding voice.
    // Patches that must retrigger on every key (e.g. PT-10 Piano) have legatoEnabled = false.
    const bool canLegato = legatoEnabled_
                        && hadHeldKeyBefore
                        && v.isActive()
                        && !v.isReleasing();

    if (target->noteNumber != noteNumber && target->noteNumber == v.getNoteNumber() &&
        v.isActive() && !v.isReleasing()) {
        return; // priority kept the sounding note (e.g. High priority, lower key pressed)
    }
    if (canLegato) {
        v.legatoNoteChange(target->noteNumber, target->velocity, portamentoEnabled_);
    } else {
        applyParametersToVoice(v);
        v.noteOn(target->noteNumber, target->velocity, ++voiceCounter_);
    }
}

void SynthEngine::noteOffMono(uint8_t rawNote) {
    removeHeldKey(rawNote);

    Voice& v = voices_[0];
    if (!heldKeys_.empty()) {
        const HeldKey* nextNote = resolveMonophonicNote();
        if (nextNote && nextNote->noteNumber != v.getNoteNumber()) {
            const bool canLegato = legatoEnabled_
                                && v.isActive()
                                && !v.isReleasing();
            if (canLegato) {
                v.legatoNoteChange(nextNote->noteNumber, nextNote->velocity, portamentoEnabled_);
            } else {
                applyParametersToVoice(v);
                v.noteOn(nextNote->noteNumber, nextNote->velocity, ++voiceCounter_);
            }
        }
    } else {
        v.releaseWithSustain(sustainPedalHeld_);
    }
}

void SynthEngine::nextPreset() {
    const auto& presets = getPresets();
    loadPresetIndex((currentPresetIndex_ + 1) % presets.size());
}

void SynthEngine::prevPreset() {
    const auto& presets = getPresets();
    loadPresetIndex((currentPresetIndex_ + presets.size() - 1) % presets.size());
}

void SynthEngine::setVoiceLimit(size_t limit) {
    voiceLimit_ = std::clamp<size_t>(limit, 1, MAX_VOICES);
}

size_t SynthEngine::getEffectiveVoiceLimit() const {
    if (voicing_.voices > 0) return std::min<size_t>(voiceLimit_, voicing_.voices);
    return voiceLimit_;
}

Voice* SynthEngine::allocateVoice(uint8_t rawNoteNumber) {
    const size_t limit = getEffectiveVoiceLimit();

    // 1. The same physical key retriggers its own voice
    for (size_t i = 0; i < limit; ++i) {
        if (voices_[i].isActive() && voices_[i].getRawNoteNumber() == rawNoteNumber) {
            return &voices_[i];
        }
    }

    // 2. Look for an idle voice
    for (size_t i = 0; i < limit; ++i) {
        if (!voices_[i].isActive()) {
            return &voices_[i];
        }
    }

    // 3. Look for a voice currently in release phase (oldest first)
    Voice* bestRelease = nullptr;
    uint32_t oldestReleaseAge = 0xFFFFFFFF;
    for (size_t i = 0; i < limit; ++i) {
        if (voices_[i].isReleasing()) {
            if (voices_[i].getAge() < oldestReleaseAge) {
                oldestReleaseAge = voices_[i].getAge();
                bestRelease = &voices_[i];
            }
        }
    }
    if (bestRelease != nullptr) {
        return bestRelease;
    }

    // 4. Voice stealing: steal the oldest active voice (LRU)
    Voice* oldestActive = &voices_[0];
    uint32_t oldestAge = voices_[0].getAge();
    for (size_t i = 1; i < limit; ++i) {
        if (voices_[i].getAge() < oldestAge) {
            oldestAge = voices_[i].getAge();
            oldestActive = &voices_[i];
        }
    }

    return oldestActive;
}

void SynthEngine::noteOn(uint8_t noteNumber, uint8_t velocity) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    if (velocity == 0) {
        noteOff(noteNumber);
        return;
    }

    lastNoteNumber_ = noteNumber;
    lastVelocity_ = velocity;
    lastNoteOnMillis_ = millis();

    if (deferredEvents_) pushEvent(EventType::NoteOn, noteNumber, velocity);
    else noteOnImpl(noteNumber, velocity);
}

void SynthEngine::noteOnImpl(uint8_t noteNumber, uint8_t velocity) {
    uint8_t effectiveNote = quantizeNoteToKeyAndScale(noteNumber);

    if (voiceMode_ == VoiceMode::Monophonic) {
        noteOnMono(effectiveNote, velocity, noteNumber);
        return;
    }

    Voice* voice = allocateVoice(noteNumber);
    if (voice != nullptr) {
        applyParametersToVoice(*voice);
        voice->noteOn(effectiveNote, velocity, ++voiceCounter_);
        voice->setRawNoteNumber(noteNumber);
    }
}

void SynthEngine::noteOff(uint8_t noteNumber) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    if (deferredEvents_) pushEvent(EventType::NoteOff, noteNumber);
    else noteOffImpl(noteNumber);
}

void SynthEngine::noteOffImpl(uint8_t noteNumber) {
    if (voiceMode_ == VoiceMode::Monophonic) {
        noteOffMono(noteNumber);
        return;
    }

    // Release only the voice(s) started by this physical key: with the scale quantizer on, two
    // keys can map to the same pitch and must not release each other
    for (size_t i = 0; i < MAX_VOICES; ++i) {
        if (voices_[i].isActive() && voices_[i].getRawNoteNumber() == noteNumber) {
            voices_[i].releaseWithSustain(sustainPedalHeld_);
        }
    }
}

void SynthEngine::controlChange(uint8_t ccNumber, uint8_t value) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    switch (ccNumber) {
        // Preset and bank navigation CCs are handled by MpkMiniMk4::ControlSurface, which keeps its knob state in step

        // Sustain Pedal (Damper)
        case CC_SUSTAIN_PEDAL:
            setSustainPedal(value >= 64);
            break;

        // Primary Controls (Bank A)
        case CC_FILTER_CUTOFF:
            if (engineMode_ == EngineMode::FM) {
                setModDepth(ParameterScaler::ccToNorm(value) * 12.0f);
            } else {
                setCutoffHz(ParameterScaler::ccToCutoffHz(value));
            }
            break;

        case CC_FILTER_RESONANCE:
            if (engineMode_ == EngineMode::FM) {
                setModRatio(0.5f + ParameterScaler::ccToNorm(value) * 13.5f);
            } else {
                setResonanceQ(ParameterScaler::ccToResonanceQ(value));
            }
            break;

        case CC_OSC_WAVEFORM:
            if (engineMode_ == EngineMode::FM) {
                setModDepth(ParameterScaler::ccToNorm(value) * 12.0f);
            } else if (isCasioModel(getCurrentPreset().model)) {
                if (value < 32) {
                    setCasioTone(CasioTone::Piano);
                } else if (value < 64) {
                    setCasioTone(CasioTone::Fantasy);
                } else if (value < 96) {
                    setCasioTone(CasioTone::Violin);
                } else {
                    setCasioTone(CasioTone::Flute);
                }
            } else {
                if (value < 43) {
                    setWaveform(Waveform::Sawtooth);
                } else if (value < 86) {
                    setWaveform(Waveform::Square);
                } else {
                    setWaveform(Waveform::Triangle);
                }
            }
            break;

        case CC_AMP_ATTACK:
            setAmpAttackMs(ParameterScaler::ccToEnvelopeTimeMs(value, 1.0f, 4000.0f));
            break;

        case CC_AMP_DECAY:
            setAmpDecayMs(ParameterScaler::ccToEnvelopeTimeMs(value, 5.0f, 5000.0f));
            break;

        case CC_AMP_SUSTAIN:
            setAmpSustainLevel(ParameterScaler::ccToNorm(value));
            break;

        case CC_AMP_RELEASE:
            setAmpReleaseMs(ParameterScaler::ccToEnvelopeTimeMs(value, 5.0f, 5000.0f));
            break;

        case CC_FILTER_ENV_AMT:
            if (engineMode_ == EngineMode::FM) {
                setFilterDecayMs(ParameterScaler::ccToEnvelopeTimeMs(value, 10.0f, 4000.0f));
            } else if (isCasioModel(getCurrentPreset().model) && shiftActive_) {
                setPulseWidth(0.05f + ParameterScaler::ccToNorm(value) * 0.90f);
            } else {
                setFilterEnvAmount(ParameterScaler::ccToNorm(value));
            }
            break;

        case CC_SUB_OSC_LEVEL: // Sub-oscillator level, or FM Feedback (CC 77)
            if (engineMode_ == EngineMode::FM) {
                setModFeedback(ParameterScaler::ccToNorm(value) * 1.5f);
            } else {
                setSubOscLevel(ParameterScaler::ccToNorm(value));
            }
            break;

        case CC_CHORUS_MIX: // Chorus mix (CC 78)
            chorus_.setEnabled(value > 10);
            chorus_.setMix(ParameterScaler::ccToNorm(value));
            break;

        case CC_DRIVE: // Pre-filter overdrive (CC 87)
            setDrive(1.0f + ParameterScaler::ccToNorm(value) * 2.5f);
            break;

        case CC_ACCENT: // TB-303 accent amount (CC 88)
            setAccentAmount(ParameterScaler::ccToNorm(value));
            break;

        case CC_LFO_RATE: // Vibrato LFO rate (CC 89)
            setVibratoRateHz(ccToVibratoRateHz(value));
            break;

        case CC_LFO_DEPTH: // Vibrato LFO depth (CC 90)
            setVibratoDepth(ParameterScaler::ccToNorm(value));
            break;

        case CC_VOICE_MODE: // Knob / switch voice mode (CC 92)
            setVoiceMode(value >= 64 ? VoiceMode::Polyphonic : VoiceMode::Monophonic);
            break;

        case CC_GLIDE: // Glide on/off + time on one control (CC 93)
            setPortamentoEnabled(value > 0);
            setGlideTimeMs(ParameterScaler::ccToNorm(value) * 1000.0f);
            break;

        case CC_CHORUS_SEND:   // External Chorus Mix (CC 91)
            chorus_.setEnabled(value > 10);
            chorus_.setMix(ParameterScaler::ccToNorm(value));
            break;

        case CC_DETUNE_DEPTH:  // External Detune Cents (CC 94)
            setDetuneCents(ParameterScaler::ccToNorm(value) * 50.0f);
            break;

        // Secondary Controls (Bank B / Shift)
        case CC_FILTER_ENV_ATTACK: // Filter Attack, PolyMod, Distortion, or FM ModSust (CC 80)
            if (engineMode_ == EngineMode::FM) {
                setFilterSustainLevel(ParameterScaler::ccToNorm(value));
            } else {
                setFilterAttackMs(ParameterScaler::ccToEnvelopeTimeMs(value, 1.0f, 4000.0f));
            }
            break;

        case CC_FILTER_ENV_DECAY: // Filter Decay, Resonance Peak, or FM ModAtt (CC 81)
            if (engineMode_ == EngineMode::FM) {
                setFilterAttackMs(ParameterScaler::ccToEnvelopeTimeMs(value, 1.0f, 2000.0f));
            } else {
                setFilterDecayMs(ParameterScaler::ccToEnvelopeTimeMs(value, 5.0f, 5000.0f));
            }
            break;

        case CC_FILTER_ENV_SUSTAIN: // Filter Sustain, SubOsc, or FM Tremolo Depth (CC 82)
            if (engineMode_ == EngineMode::FM) {
                setTremoloDepth(ParameterScaler::ccToNorm(value));
            } else if (isCasioModel(getCurrentPreset().model) && shiftActive_) {
                setSubOscLevel(ParameterScaler::ccToNorm(value));
            } else {
                setFilterSustainLevel(ParameterScaler::ccToNorm(value));
            }
            break;

        case CC_FILTER_ENV_RELEASE: // Filter Release, SubBass, or FM Tremolo Rate (CC 83)
            if (engineMode_ == EngineMode::FM) {
                setTremoloRateHz(0.5f + ParameterScaler::ccToNorm(value) * 15.0f);
            } else {
                setFilterReleaseMs(ParameterScaler::ccToEnvelopeTimeMs(value, 5.0f, 5000.0f));
            }
            break;

        case CC_CHORUS_RATE: // Chorus Rate (CC 84); vibrato rate is CC 89
            chorus_.setRateHz(0.1f + ParameterScaler::ccToNorm(value) * 4.9f);
            break;

        case CC_CHORUS_DEPTH: // Chorus Depth (CC 85); vibrato depth is CC 90
            chorus_.setDepthMs(0.5f + ParameterScaler::ccToNorm(value) * 4.5f);
            break;

        // Direct Hardware Controller Knobs K1..K8 (defined in MidiMapping.h per controller profile)
        case CC_HW_KNOB_1:
        case CC_HW_KNOB_2:
        case CC_HW_KNOB_3:
        case CC_HW_KNOB_4:
        case CC_HW_KNOB_5:
        case CC_HW_KNOB_6:
        case CC_HW_KNOB_7:
        case CC_HW_KNOB_8: {
            const uint8_t kIdx = static_cast<uint8_t>(ccNumber - CC_HW_KNOB_BASE);
            const auto& preset = getCurrentPreset();
            const uint8_t targetCc = (shiftActive_ ? preset.bankB : preset.bankA).ccs[kIdx];
            if (targetCc < CC_HW_KNOB_1 || targetCc > CC_HW_KNOB_8) {
                controlChange(targetCc, value);
            }
            break;
        }

        // Direct FM Synthesis CCs (CC_FM_MOD_DEPTH..CC_FM_MASTER_VOL)
        case CC_FM_MOD_DEPTH:
            if (engineMode_ == EngineMode::FM) setModDepth(ParameterScaler::ccToNorm(value) * 12.0f);
            break;

        case CC_FM_MOD_RATIO:
            if (engineMode_ == EngineMode::FM) setModRatio(0.5f + ParameterScaler::ccToNorm(value) * 13.5f);
            break;

        case CC_FM_FILTER_DECAY:
            if (engineMode_ == EngineMode::FM) setFilterDecayMs(ParameterScaler::ccToEnvelopeTimeMs(value, 10.0f, 4000.0f));
            break;

        case CC_FM_AMP_RELEASE:
            if (engineMode_ == EngineMode::FM) setAmpReleaseMs(ParameterScaler::ccToEnvelopeTimeMs(value, 5.0f, 5000.0f));
            break;

        case CC_FM_FEEDBACK:
            if (engineMode_ == EngineMode::FM) setModFeedback(ParameterScaler::ccToNorm(value) * 1.5f);
            break;

        case CC_FM_CHORUS_MIX:
            if (engineMode_ == EngineMode::FM) {
                chorus_.setEnabled(value > 10);
                chorus_.setMix(ParameterScaler::ccToNorm(value));
            }
            break;

        case CC_FM_TREMOLO_DEPTH:
            if (engineMode_ == EngineMode::FM) setTremoloDepth(ParameterScaler::ccToNorm(value));
            break;

        case CC_FM_MASTER_VOL:
            if (engineMode_ == EngineMode::FM) setMasterVolume(ParameterScaler::ccToNorm(value));
            break;

        // Wavetable / phase distortion / pluck / additive engines
        case CC_WT_POSITION:   setWavetablePosition(ParameterScaler::ccToNorm(value)); break;
        case CC_WT_ENV_AMT:    setWavetableEnvAmount(ParameterScaler::ccToNorm(value)); break;
        case CC_WT_TABLE:      setWavetable(static_cast<uint8_t>(value * WAVETABLE_COUNT / 128)); break;
        case CC_PD_DCW:        setPdDcw(ParameterScaler::ccToNorm(value)); break;
        case CC_PD_WAVE:
            setPdWave(static_cast<PdWave>(value * static_cast<uint8_t>(PdWave::Count) / 128));
            break;
        case CC_PLUCK_DECAY:   setPluckDecayMs(ParameterScaler::ccToEnvelopeTimeMs(value, 100.0f, 10000.0f)); break;
        case CC_PLUCK_BRIGHT:  setPluckBrightness(ParameterScaler::ccToNorm(value)); break;
        case CC_PLUCK_PICK:    setPluckPickPosition(ParameterScaler::ccToNorm(value)); break;
        case CC_PERC_LEVEL:    setPercussionLevel(ParameterScaler::ccToNorm(value)); break;
        case CC_PERC_HARMONIC: setPercussionHarmonic(value >= 64 ? 1 : 0); break;
        case CC_KEY_CLICK:     setKeyClick(ParameterScaler::ccToNorm(value)); break;
        case CC_TREMOLO_DEPTH: setTremoloDepth(ParameterScaler::ccToNorm(value)); break;
        case CC_TREMOLO_RATE:  setTremoloRateHz(0.5f + ParameterScaler::ccToNorm(value) * 15.0f); break;
        case CC_DRAWBAR_1:
        case CC_DRAWBAR_1 + 1:
        case CC_DRAWBAR_1 + 2:
        case CC_DRAWBAR_1 + 3:
        case CC_DRAWBAR_1 + 4:
        case CC_DRAWBAR_1 + 5:
        case CC_DRAWBAR_1 + 6:
        case CC_DRAWBAR_1 + 7:
        case CC_DRAWBAR_9:     // nine detented steps, like the drawbars' 0..8 markings
            setDrawbar(ccNumber - CC_DRAWBAR_1, std::round(ParameterScaler::ccToNorm(value) * 8.0f));
            break;

        case CC_MOD_WHEEL:
            modWheel_ = ParameterScaler::ccToNorm(value);
            break;

        case CC_VOLUME:
            setMasterVolume(ParameterScaler::ccToNorm(value));
            break;

        case CC_PORTAMENTO_TIME: // Portamento Time (0 .. 1000 ms)
            setGlideTimeMs(ParameterScaler::ccToNorm(value) * 1000.0f);
            break;

        case CC_PORTAMENTO_ENABLE: // Portamento On/Off switch (<64 off, >=64 on) & TB-303 Slide
            setPortamentoEnabled(value >= 64);
            if (getCurrentPreset().model == SynthModel::TB303 && value > 0) {
                setGlideTimeMs(ParameterScaler::ccToNorm(value) * 200.0f);
            }
            break;

        case CC_MONO_MODE: // Mono Mode On
            setVoiceMode(VoiceMode::Monophonic);
            break;

        case CC_POLY_MODE: // Poly Mode On
            setVoiceMode(VoiceMode::Polyphonic);
            break;

        case CC_ALL_SOUND_OFF:
            panic();
            break;

        case CC_ALL_NOTES_OFF:
            allNotesOff();
            break;

        // Metronome, Tempo, Time Signature & Musical Key / Scale CCs
        case CC_METRO_VOLUME:
            if (value == 0) {
                setMetronomeEnabled(false);
            } else {
                setMetronomeVolume(ParameterScaler::ccToNorm(value));
                if (!metronomeEnabled_) setMetronomeEnabled(true);
            }
            break;

        case CC_TEMPO_BPM:
            setMetronomeBpm(40.0f + ParameterScaler::ccToNorm(value) * 200.0f);
            break;

        case CC_TIME_SIGNATURE: {
            uint8_t idx = static_cast<uint8_t>((static_cast<uint16_t>(value) * static_cast<uint16_t>(TimeSignaturePreset::Count)) / 128);
            setTimeSignaturePreset(static_cast<TimeSignaturePreset>(idx));
            break;
        }

        case CC_TIME_SIG_BEATS: {
            uint8_t beats = static_cast<uint8_t>(1 + (static_cast<uint16_t>(value) * 15) / 127);
            setTimeSignature(beats, timeSigDenominator_);
            break;
        }

        case CC_ROOT_KEY: {
            uint8_t key = static_cast<uint8_t>((static_cast<uint16_t>(value) * 12) / 128);
            setRootKey(key);
            break;
        }

        case CC_SCALE_MODE: {
            uint8_t modeIdx = static_cast<uint8_t>((static_cast<uint16_t>(value) * static_cast<uint16_t>(ScaleMode::Count)) / 128);
            setScaleMode(static_cast<ScaleMode>(modeIdx));
            break;
        }

        case CC_METRO_SILENT:
            setMetronomeSilent(value >= 64);
            break;

        case CC_METRO_KEY_TUNE:
            setMetronomeKeyTuned(value >= 64);
            break;

        case CC_METRO_CLICK: {
            // 0 = woodblock, 1..12 = click tuned to C..B
            const uint8_t step = static_cast<uint8_t>((static_cast<uint16_t>(value) * 13) / 128);
            setMetronomeKeyTuned(step > 0);
            if (step > 0) setRootKey(static_cast<uint8_t>(step - 1));
            break;
        }

        case CC_OUTPUT_MODE:
            setMonoOutput(value >= 64);
            break;

        case CC_METRONOME_TOGGLE:
            if (value >= 64) {
                toggleMetronome();
            }
            break;

        case CC_TAP_TEMPO:
            if (value >= 64) {
                if (shiftActive_) {
                    toggleMetronome();
                } else {
                    // Use sample-derived millisecond counter if called via CC
                    uint32_t approxMs = static_cast<uint32_t>((static_cast<double>(metroSampleCounter_) * 1000.0) / sampleRate_);
                    tapTempo(lastTapTimeMs_ + (approxMs > 150 ? approxMs : 500));
                }
            }
            break;

        default:
            break;
    }

    if (ccNumber < CC_HW_KNOB_1 || ccNumber > CC_HW_KNOB_8) {
        notifyMpkMiniParameterChanged(ccNumber, value);
    }
}

void SynthEngine::pitchBend(int16_t bendValue) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    // 14-bit bend: 0 .. 16383, center 8192 (+/- 2 semitones default)
    float normalized = (static_cast<float>(bendValue) - 8192.0f) / 8192.0f;
    // Voices pick this up (plus vibrato) every sample in renderInternal()
    pitchBendSemitones_ = normalized * 2.0f;
}

void SynthEngine::allNotesOff() {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    if (deferredEvents_) pushEvent(EventType::AllNotesOff);
    else allNotesOffImpl();
}

void SynthEngine::allNotesOffImpl() {
    heldKeys_.clear();
    sustainPedalHeld_ = false;
    for (auto& voice : voices_) {
        voice.noteOff();
    }
}

void SynthEngine::panic() {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    if (deferredEvents_) pushEvent(EventType::Panic);
    else panicImpl();
}

void SynthEngine::panicImpl() {
    heldKeys_.clear();
    sustainPedalHeld_ = false;
    for (auto& voice : voices_) {
        voice.kill();
    }
    chorus_.reset();
}

void SynthEngine::fastReleaseAllImpl() {
    heldKeys_.clear();
    for (auto& voice : voices_) {
        voice.fastRelease();
    }
}

void SynthEngine::setDeferredEvents(bool deferred) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    if (deferredEvents_ && !deferred) {
        drainEvents();
    }
    deferredEvents_ = deferred;
}

void SynthEngine::pushEvent(EventType type, uint8_t a, uint8_t b) {
    const uint32_t w = eventWrite_.load(std::memory_order_relaxed);
    if (w - eventRead_.load(std::memory_order_acquire) >= EVENT_QUEUE_SIZE) {
        return; // queue full: drop (256 events per audio block is far beyond any MIDI burst)
    }
    eventQueue_[w & (EVENT_QUEUE_SIZE - 1)] = {type, a, b};
    eventWrite_.store(w + 1, std::memory_order_release);
}

void SynthEngine::drainEvents() {
    if (voicesDirty_.exchange(false, std::memory_order_acq_rel)) {
        applyParametersToAllVoices();
    }
    uint32_t r = eventRead_.load(std::memory_order_relaxed);
    const uint32_t w = eventWrite_.load(std::memory_order_acquire);
    while (r != w) {
        dispatchEvent(eventQueue_[r & (EVENT_QUEUE_SIZE - 1)]);
        ++r;
    }
    eventRead_.store(r, std::memory_order_release);
}

void SynthEngine::dispatchEvent(const PendingEvent& e) {
    switch (e.type) {
        case EventType::NoteOn:         noteOnImpl(e.a, e.b); break;
        case EventType::NoteOff:        noteOffImpl(e.a); break;
        case EventType::AllNotesOff:    allNotesOffImpl(); break;
        case EventType::Panic:          panicImpl(); break;
        case EventType::Sustain:        setSustainPedalImpl(e.a != 0); break;
        case EventType::FastReleaseAll: fastReleaseAllImpl(); break;
    }
}

void SynthEngine::updateAllVoices() {
    if (deferredEvents_) voicesDirty_.store(true, std::memory_order_release);
    else applyParametersToAllVoices();
}

void SynthEngine::setAccentAmount(float amount) {
    accentAmount_ = std::clamp(amount, 0.0f, 1.0f);
    updateVoices([&](Voice& v) { v.setAccentAmount(accentAmount_); });
}

void SynthEngine::applyParametersToVoice(Voice& voice) {
    voice.setEngineMode(engineMode_);
    voice.setFilterModel(voicing_.filter);
    voice.setKeyTracking(voicing_.keyTrack);
    voice.setVelocityToAmp(voicing_.velToAmp);
    voice.setAccentEnabled(voicing_.accent);
    voice.setAccentAmount(accentAmount_);
    voice.setOscKeySync(voicing_.oscKeySync);
    voice.setAnalogDrift(voicing_.analogDrift);
    voice.setModRatio(modRatio_);
    voice.setModDepth(modDepth_);
    voice.setModFeedback(modFeedback_);
    voice.setTimbre(timbre_);

    voice.setWaveform(waveform_);
    voice.setPulseWidth(pulseWidth_);
    voice.setSubOscLevel(subOscLevel_);
    voice.setDetuneCents(detuneCents_);
    voice.setDrive(drive_);
    voice.setBaseCutoffHz(cutoffHz_);
    voice.setResonanceQ(resonanceQ_);
    voice.setFilterEnvAmount(filterEnvAmount_);
    voice.setPitchBend(pitchBendSemitones_);
    voice.setGlideTimeMs(glideTimeMs_);
    voice.setPortamentoEnabled(portamentoEnabled_);

    voice.setAmpAttackMs(ampAttackMs_);
    voice.setAmpDecayMs(ampDecayMs_);
    voice.setAmpSustainLevel(ampSustainLevel_);
    voice.setAmpReleaseMs(ampReleaseMs_);

    voice.setFilterAttackMs(filterAttackMs_);
    voice.setFilterDecayMs(filterDecayMs_);
    voice.setFilterSustainLevel(filterSustainLevel_);
    voice.setFilterReleaseMs(filterReleaseMs_);
}

void SynthEngine::setModRatio(float ratio) {
    modRatio_ = std::clamp(ratio, 0.25f, 32.0f);
    updateVoices([&](Voice& v) { v.setModRatio(modRatio_); });
}

void SynthEngine::setModDepth(float depth) {
    modDepth_ = std::clamp(depth, 0.0f, 20.0f);
    updateVoices([&](Voice& v) { v.setModDepth(modDepth_); });
}

void SynthEngine::setModFeedback(float fb) {
    modFeedback_ = std::clamp(fb, 0.0f, 3.0f);
    updateVoices([&](Voice& v) { v.setModFeedback(modFeedback_); });
}

void SynthEngine::setTremoloDepth(float depth) {
    tremoloDepth_ = std::clamp(depth, 0.0f, 1.0f);
}

void SynthEngine::setTremoloRateHz(float rateHz) {
    tremoloRateHz_ = std::clamp(rateHz, 0.1f, 20.0f);
}

const char* SynthEngine::getEngineModeName(EngineMode mode) {
    switch (mode) {
        case EngineMode::FM:        return "FM";
        case EngineMode::Wavetable: return "WAVETABLE";
        case EngineMode::PhaseDist: return "PHASE DIST";
        case EngineMode::Pluck:     return "PLUCK";
        case EngineMode::Additive:  return "ADDITIVE";
        default:                    return "SUBTRACTIVE";
    }
}

void SynthEngine::setTimbre(const TimbreParams& timbre) {
    timbre_ = timbre;
    timbre_.wtTable      = std::min<uint8_t>(timbre_.wtTable, static_cast<uint8_t>(WAVETABLE_COUNT - 1));
    timbre_.wtPosition   = std::clamp(timbre_.wtPosition, 0.0f, 1.0f);
    timbre_.wtEnvAmount  = std::clamp(timbre_.wtEnvAmount, 0.0f, 1.0f);
    if (static_cast<uint8_t>(timbre_.pdWave) >= static_cast<uint8_t>(PdWave::Count)) timbre_.pdWave = PdWave::Saw;
    timbre_.pdDcw        = std::clamp(timbre_.pdDcw, 0.0f, 1.0f);
    timbre_.pluckDecayMs = std::clamp(timbre_.pluckDecayMs, 100.0f, 10000.0f);
    timbre_.pluckBright  = std::clamp(timbre_.pluckBright, 0.0f, 1.0f);
    timbre_.pluckPick    = std::clamp(timbre_.pluckPick, 0.0f, 1.0f);
    for (float& bar : timbre_.drawbars) bar = std::clamp(bar, 0.0f, 8.0f);
    timbre_.percLevel    = std::clamp(timbre_.percLevel, 0.0f, 1.0f);
    timbre_.percHarmonic = timbre_.percHarmonic ? 1 : 0;
    timbre_.keyClick     = std::clamp(timbre_.keyClick, 0.0f, 1.0f);
    updateVoices([&](Voice& v) { v.setTimbre(timbre_); });
}

void SynthEngine::setWavetablePosition(float pos)      { editTimbre([&](TimbreParams& t) { t.wtPosition = pos; }); }
void SynthEngine::setWavetableEnvAmount(float amount)  { editTimbre([&](TimbreParams& t) { t.wtEnvAmount = amount; }); }
void SynthEngine::setWavetable(uint8_t table)          { editTimbre([&](TimbreParams& t) { t.wtTable = table; }); }
void SynthEngine::setPdDcw(float dcw)                  { editTimbre([&](TimbreParams& t) { t.pdDcw = dcw; }); }
void SynthEngine::setPdWave(PdWave wave)               { editTimbre([&](TimbreParams& t) { t.pdWave = wave; }); }
void SynthEngine::setPluckDecayMs(float ms)            { editTimbre([&](TimbreParams& t) { t.pluckDecayMs = ms; }); }
void SynthEngine::setPluckBrightness(float bright)     { editTimbre([&](TimbreParams& t) { t.pluckBright = bright; }); }
void SynthEngine::setPluckPickPosition(float pick)     { editTimbre([&](TimbreParams& t) { t.pluckPick = pick; }); }
void SynthEngine::setPercussionLevel(float level)      { editTimbre([&](TimbreParams& t) { t.percLevel = level; }); }
void SynthEngine::setPercussionHarmonic(uint8_t harm)  { editTimbre([&](TimbreParams& t) { t.percHarmonic = harm; }); }
void SynthEngine::setKeyClick(float level)             { editTimbre([&](TimbreParams& t) { t.keyClick = level; }); }
void SynthEngine::setDrawbar(size_t index, float level) {
    if (index < DRAWBAR_COUNT) editTimbre([&](TimbreParams& t) { t.drawbars[index] = level; });
}

void SynthEngine::setSustainPedal(bool held) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    if (deferredEvents_) pushEvent(EventType::Sustain, held ? 1 : 0);
    else setSustainPedalImpl(held);
}

void SynthEngine::setSustainPedalImpl(bool held) {
    sustainPedalHeld_ = held;
    if (!held) {
        for (auto& voice : voices_) {
            voice.releaseSustainPedal();
        }
    }
}

void SynthEngine::applyParametersToAllVoices() {
    for (auto& voice : voices_) {
        applyParametersToVoice(voice);
    }
}

void SynthEngine::setCutoffHz(float cutoffHz) {
    cutoffHz_ = std::clamp(cutoffHz, 20.0f, 20000.0f);
    updateVoices([&](Voice& v) { v.setBaseCutoffHz(cutoffHz_); });
}

void SynthEngine::setResonanceQ(float q) {
    resonanceQ_ = std::clamp(q, 0.5f, 25.0f);
    updateVoices([&](Voice& v) { v.setResonanceQ(resonanceQ_); });
}

void SynthEngine::setWaveform(Waveform wave) {
    waveform_ = wave;
    updateVoices([&](Voice& v) { v.setWaveform(wave); });
}

void SynthEngine::setSubOscLevel(float lvl) {
    subOscLevel_ = lvl;
    updateVoices([&](Voice& v) { v.setSubOscLevel(lvl); });
}

void SynthEngine::setDetuneCents(float cents) {
    detuneCents_ = cents;
    updateVoices([&](Voice& v) { v.setDetuneCents(cents); });
}

void SynthEngine::setDrive(float drive) {
    drive_ = drive;
    updateVoices([&](Voice& v) { v.setDrive(drive); });
}

void SynthEngine::setChorusEnabled(bool enabled) {
    chorus_.setEnabled(enabled);
}

void SynthEngine::setChorusMix(float mix) {
    chorus_.setMix(mix);
}

void SynthEngine::setAmpAttackMs(float ms) {
    ampAttackMs_ = ms;
    updateVoices([&](Voice& v) { v.setAmpAttackMs(ms); });
}

void SynthEngine::setAmpDecayMs(float ms) {
    ampDecayMs_ = ms;
    updateVoices([&](Voice& v) { v.setAmpDecayMs(ms); });
}

void SynthEngine::setAmpSustainLevel(float level) {
    ampSustainLevel_ = level;
    updateVoices([&](Voice& v) { v.setAmpSustainLevel(level); });
}

void SynthEngine::setAmpReleaseMs(float ms) {
    ampReleaseMs_ = ms;
    updateVoices([&](Voice& v) { v.setAmpReleaseMs(ms); });
}

void SynthEngine::setFilterAttackMs(float ms) {
    filterAttackMs_ = ms;
    updateVoices([&](Voice& v) { v.setFilterAttackMs(ms); });
}

void SynthEngine::setFilterDecayMs(float ms) {
    filterDecayMs_ = ms;
    updateVoices([&](Voice& v) { v.setFilterDecayMs(ms); });
}

void SynthEngine::setFilterSustainLevel(float level) {
    filterSustainLevel_ = level;
    updateVoices([&](Voice& v) { v.setFilterSustainLevel(level); });
}

void SynthEngine::setFilterReleaseMs(float ms) {
    filterReleaseMs_ = ms;
    updateVoices([&](Voice& v) { v.setFilterReleaseMs(ms); });
}

void SynthEngine::setFilterEnvAmount(float amount) {
    filterEnvAmount_ = amount;
    updateVoices([&](Voice& v) { v.setFilterEnvAmount(amount); });
}

void SynthEngine::setMasterVolume(float vol) {
    masterGain_ = std::clamp(vol, 0.0f, 1.0f);
    outputGain_ = masterGain_ * levelGain_;
}

void SynthEngine::setPulseWidth(float pw) {
    pulseWidth_ = std::clamp(pw, 0.05f, 0.95f);
    updateVoices([&](Voice& v) { v.setPulseWidth(pulseWidth_); });
}

void SynthEngine::setVibratoDepth(float depth) {
    vibratoDepth_ = std::clamp(depth, 0.0f, 1.0f);
}

void SynthEngine::setVibratoRateHz(float rateHz) {
    lfoRateHz_ = std::clamp(rateHz, 0.1f, 20.0f);
}

void SynthEngine::setCasioTone(CasioTone tone) {
    casioTone_ = tone;
    switch (tone) {
        case CasioTone::Piano:
            waveform_        = Waveform::Square;
            pulseWidth_      = 0.70f;  // ~70% duty cycle asymmetric pulse
            subOscLevel_     = 0.0f;
            cutoffHz_        = 3800.0f; // Snappy lo-fi RC reconstruction filter
            resonanceQ_      = 0.8f;
            filterEnvAmount_ = 0.0f;
            ampAttackMs_     = 2.0f;   // Snappy percussive key strike
            ampDecayMs_      = 450.0f; // Natural decay to zero
            ampSustainLevel_ = 0.0f;   // Zero sustain
            ampReleaseMs_    = 60.0f;
            vibratoDepth_    = 0.0f;   // Dry
            drive_           = 1.10f;
            break;

        case CasioTone::Fantasy:
            waveform_        = Waveform::Square;
            pulseWidth_      = 0.50f;  // 50% pure square wave
            subOscLevel_     = 0.35f;  // Octave harmonic overtone
            cutoffHz_        = 4800.0f;
            resonanceQ_      = 0.8f;
            filterEnvAmount_ = 0.0f;
            ampAttackMs_     = 10.0f;  // Fast synth lead attack
            ampDecayMs_      = 350.0f;
            ampSustainLevel_ = 0.85f;  // Sustained signature lead
            ampReleaseMs_    = 250.0f;
            lfoRateHz_       = 5.5f;   // Casio ~5.5 Hz vibrato
            vibratoDepth_    = 0.25f;  // Characteristic gentle wobble
            drive_           = 1.15f;
            break;

        case CasioTone::Violin:
            waveform_        = Waveform::Square;
            pulseWidth_      = 0.22f;  // Narrow reedy pulse simulating multipulse violin
            subOscLevel_     = 0.0f;
            cutoffHz_        = 4200.0f;
            resonanceQ_      = 0.9f;
            filterEnvAmount_ = 0.0f;
            ampAttackMs_     = 45.0f;  // Bowed string swell attack
            ampDecayMs_      = 200.0f;
            ampSustainLevel_ = 0.90f;  // Full sustain
            ampReleaseMs_    = 180.0f;
            lfoRateHz_       = 5.0f;
            vibratoDepth_    = 0.15f;  // Moderate string vibrato
            drive_           = 1.15f;
            break;

        case CasioTone::Flute:
            waveform_        = Waveform::Square;
            pulseWidth_      = 0.50f;  // 50% square wave
            subOscLevel_     = 0.0f;
            cutoffHz_        = 1400.0f;// Mellow low-pass filter
            resonanceQ_      = 0.7f;
            filterEnvAmount_ = 0.0f;
            ampAttackMs_     = 30.0f;  // Breath attack
            ampDecayMs_      = 200.0f;
            ampSustainLevel_ = 0.90f;  // Full sustain
            ampReleaseMs_    = 150.0f;
            lfoRateHz_       = 5.2f;
            vibratoDepth_    = 0.10f;  // Gentle flute vibrato
            drive_           = 1.05f;
            break;

        default:
            break;
    }
    updateAllVoices();
}

const char* SynthEngine::getCasioToneName() const {
    switch (casioTone_) {
        case CasioTone::Piano:   return "PIANO";
        case CasioTone::Fantasy: return "FANTASY";
        case CasioTone::Violin:  return "VIOLIN";
        case CasioTone::Flute:   return "FLUTE";
        default:                 return "UNKNOWN";
    }
}

size_t SynthEngine::getActiveVoiceCount() const {
    size_t activeLimit = (voiceMode_ == VoiceMode::Monophonic) ? 1 : getEffectiveVoiceLimit();
    size_t count = 0;
    for (size_t i = 0; i < activeLimit; ++i) {
        if (voices_[i].isActive()) count++;
    }
    return count;
}

// =========================================================================
// Metronome, MIDI Clock (24 PPQN), Time Signature & Musical Key/Scale
// =========================================================================

bool SynthEngine::isSecondaryCompoundAccent(uint8_t beatIdx) const {
    if (beatIdx == 0) return false;
    if (timeSigDenominator_ == 8) {
        if (timeSigNumerator_ == 6 && beatIdx == 3) return true;
        if (timeSigNumerator_ == 7 && (beatIdx == 3 || beatIdx == 5)) return true;
        if (timeSigNumerator_ == 9 && (beatIdx == 3 || beatIdx == 6)) return true;
        if (timeSigNumerator_ == 12 && (beatIdx == 3 || beatIdx == 6 || beatIdx == 9)) return true;
    } else if (timeSigNumerator_ == 5 && beatIdx == 3) {
        return true; // 5/4 (3+2 grouping)
    }
    return false;
}

void SynthEngine::triggerMetroClick(uint8_t beatIdx) {
    if (!metronomeEnabled_) return;
    if (!metronomeSilent_ && metronomeVolume_ <= 0.001f) return;

    lastMetroClickDownbeat_ = (beatIdx == 0);
    ++metroClickCount_;
    if (metronomeSilent_) return; // beat is counted for the LEDs, but no click sound
    metroPhase_ = 0.0f;
    // Fast 4ms pitch drop transient for woodblock / MPC click punch
    metroPitchEnv_   = 0.55f;
    metroPitchDecay_ = std::exp(-1.0f / std::max(1.0f, 0.004f * sampleRate_));

    if (beatIdx == 0) {
        // Beat 1 (Downbeat): Strong high accent
        metroClickEnv_   = 1.0f;
        metroClickDecay_ = std::exp(-1.0f / std::max(1.0f, 0.060f * sampleRate_));
        if (metronomeKeyTuned_) {
            // Tuned to Root Key in Octave 6 (MIDI 84..95: C6 = 1046.5 Hz .. B6 = 1975.5 Hz)
            metroBaseFreq_ = ParameterScaler::midiNoteToHz(static_cast<uint8_t>(84 + (rootKey_ % 12)));
        } else {
            metroBaseFreq_ = 1600.0f;
        }
    } else if (isSecondaryCompoundAccent(beatIdx)) {
        // Secondary compound beat (e.g. beat 4 in 6/8): Medium accent
        metroClickEnv_   = 0.85f;
        metroClickDecay_ = std::exp(-1.0f / std::max(1.0f, 0.050f * sampleRate_));
        if (metronomeKeyTuned_) {
            // Perfect 5th above Root Key in Octave 5
            metroBaseFreq_ = ParameterScaler::midiNoteToHz(static_cast<uint8_t>(72 + (rootKey_ % 12) + 7));
        } else {
            metroBaseFreq_ = 1200.0f;
        }
    } else {
        // Standard sub-beat (Beats 2, 3, 4...): Unaccented click
        metroClickEnv_   = 0.75f;
        metroClickDecay_ = std::exp(-1.0f / std::max(1.0f, 0.045f * sampleRate_));
        if (metronomeKeyTuned_) {
            // Tuned to Root Key in Octave 5 (MIDI 72..83: C5 = 523.25 Hz .. B5 = 987.77 Hz)
            metroBaseFreq_ = ParameterScaler::midiNoteToHz(static_cast<uint8_t>(72 + (rootKey_ % 12)));
        } else {
            metroBaseFreq_ = 880.0f;
        }
    }
}

void SynthEngine::setMetronomeEnabled(bool enabled) {
    if (metronomeEnabled_ == enabled) return;
    metronomeEnabled_ = enabled;
    if (metronomeEnabled_) {
        currentBeatIndex_   = 0;
        metroSampleCounter_ = 0;
        midiClockTickCount_ = 0;
        triggerMetroClick(0); // Immediate Beat 1 downbeat click on key press
    } else {
        metroClickEnv_ = 0.0f;
    }
}

void SynthEngine::setMetronomeSilent(bool silent) {
    metronomeSilent_ = silent;
    if (silent) metroClickEnv_ = 0.0f; // cut a click that is still ringing
}

void SynthEngine::toggleMetronome() {
    setMetronomeEnabled(!metronomeEnabled_);
}

void SynthEngine::setMetronomeVolume(float vol) {
    metronomeVolume_ = std::clamp(vol, 0.0f, 1.0f);
}

void SynthEngine::setMetronomeBpm(float bpm) {
    metronomeBpm_ = std::clamp(bpm, 40.0f, 240.0f);
}

void SynthEngine::tapTempo(uint32_t nowMs) {
    if (lastTapTimeMs_ > 0 && nowMs > lastTapTimeMs_) {
        uint32_t deltaMs = nowMs - lastTapTimeMs_;
        // Valid tap interval between 250ms (240 BPM) and 1500ms (40 BPM)
        if (deltaMs >= 250 && deltaMs <= 1500) {
            float tappedBpm = 60000.0f / static_cast<float>(deltaMs);
            setMetronomeBpm(tappedBpm);
            // Reset beat phase to downbeat on tap
            metroSampleCounter_ = 0;
            currentBeatIndex_   = 0;
            if (metronomeEnabled_) {
                triggerMetroClick(0);
            }
        }
    }
    lastTapTimeMs_ = nowMs;
}

void SynthEngine::midiClockStart() {
    externalClockActive_   = true;
    midiClockTickCount_    = 0;
    currentBeatIndex_      = 0;
    samplesSinceLastClock_ = 0;
}

void SynthEngine::midiClockStop() {
    externalClockActive_ = false;
    midiClockTickCount_  = 0;
}

void SynthEngine::midiClockTick() {
    // Estimate BPM from 24 PPQN inter-tick sample distance
    if (samplesSinceLastClock_ > 0) {
        float quarterNoteSamples = static_cast<float>(samplesSinceLastClock_) * 24.0f;
        float instBpm = (60.0f * sampleRate_) / quarterNoteSamples;
        if (instBpm >= 40.0f && instBpm <= 240.0f) {
            metronomeBpm_ = 0.85f * metronomeBpm_ + 0.15f * instBpm;
        }
    }
    samplesSinceLastClock_ = 0;
    externalClockActive_   = true;

    uint8_t ticksPerBeat = (timeSigDenominator_ == 8) ? 12 : 24;
    if (midiClockTickCount_ == 0) {
        if (metronomeEnabled_) {
            triggerMetroClick(currentBeatIndex_);
        }
    }

    midiClockTickCount_++;
    if (midiClockTickCount_ >= ticksPerBeat) {
        midiClockTickCount_ = 0;
        currentBeatIndex_   = static_cast<uint8_t>((currentBeatIndex_ + 1) % timeSigNumerator_);
    }
}

void SynthEngine::setTimeSignature(uint8_t beatsPerBar, uint8_t beatUnit) {
    timeSigNumerator_ = std::clamp<uint8_t>(beatsPerBar, 1, 16);
    if (beatUnit == 2 || beatUnit == 4 || beatUnit == 8 || beatUnit == 16) {
        timeSigDenominator_ = beatUnit;
    } else {
        timeSigDenominator_ = 4;
    }
    if (currentBeatIndex_ >= timeSigNumerator_) {
        currentBeatIndex_ = 0;
    }
}

void SynthEngine::setTimeSignaturePreset(TimeSignaturePreset preset) {
    switch (preset) {
        case TimeSignaturePreset::TS_4_4:  setTimeSignature(4, 4);  break;
        case TimeSignaturePreset::TS_3_4:  setTimeSignature(3, 4);  break;
        case TimeSignaturePreset::TS_2_4:  setTimeSignature(2, 4);  break;
        case TimeSignaturePreset::TS_5_4:  setTimeSignature(5, 4);  break;
        case TimeSignaturePreset::TS_6_8:  setTimeSignature(6, 8);  break;
        case TimeSignaturePreset::TS_7_8:  setTimeSignature(7, 8);  break;
        case TimeSignaturePreset::TS_9_8:  setTimeSignature(9, 8);  break;
        case TimeSignaturePreset::TS_12_8: setTimeSignature(12, 8); break;
        default:                           setTimeSignature(4, 4);  break;
    }
}

const char* SynthEngine::getTimeSignatureString() const {
    if (timeSigDenominator_ == 4) {
        switch (timeSigNumerator_) {
            case 2: return "2/4";
            case 3: return "3/4";
            case 4: return "4/4";
            case 5: return "5/4";
            case 6: return "6/4";
            case 7: return "7/4";
            default: return "4/4";
        }
    } else if (timeSigDenominator_ == 8) {
        switch (timeSigNumerator_) {
            case 5:  return "5/8";
            case 6:  return "6/8";
            case 7:  return "7/8";
            case 9:  return "9/8";
            case 12: return "12/8";
            default: return "6/8";
        }
    }
    return "4/4";
}

void SynthEngine::setRootKey(uint8_t rootKeySemitone) {
    rootKey_ = rootKeySemitone % 12;
}

const char* SynthEngine::getRootKeyName() const {
    static const char* const kKeyNames[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return kKeyNames[rootKey_ % 12];
}

void SynthEngine::setScaleMode(ScaleMode mode) {
    if (static_cast<uint8_t>(mode) < static_cast<uint8_t>(ScaleMode::Count)) {
        scaleMode_ = mode;
    } else {
        scaleMode_ = ScaleMode::Chromatic;
    }
}

const char* SynthEngine::getScaleModeName() const {
    switch (scaleMode_) {
        case ScaleMode::Chromatic:       return "CHROM";
        case ScaleMode::Major:           return "MAJOR";
        case ScaleMode::NaturalMinor:    return "MINOR";
        case ScaleMode::HarmonicMinor:   return "H-MIN";
        case ScaleMode::Dorian:          return "DORIAN";
        case ScaleMode::Mixolydian:      return "MIXOLY";
        case ScaleMode::MajorPentatonic: return "MAJ-P";
        case ScaleMode::MinorPentatonic: return "MIN-P";
        case ScaleMode::Blues:           return "BLUES";
        default:                         return "CHROM";
    }
}

uint8_t SynthEngine::quantizeNoteToKeyAndScale(uint8_t midiNote) const {
    if (scaleMode_ == ScaleMode::Chromatic) {
        return midiNote;
    }

    // 12-bit scale degree masks (bit s = 1 if semitone s above root is in scale)
    uint16_t mask = 0x0FFF;
    switch (scaleMode_) {
        case ScaleMode::Major:           mask = 0b101010110101; break; // 0,2,4,5,7,9,11
        case ScaleMode::NaturalMinor:    mask = 0b010110101101; break; // 0,2,3,5,7,8,10
        case ScaleMode::HarmonicMinor:   mask = 0b100110101101; break; // 0,2,3,5,7,8,11
        case ScaleMode::Dorian:          mask = 0b011010101101; break; // 0,2,3,5,7,9,10
        case ScaleMode::Mixolydian:      mask = 0b011010110101; break; // 0,2,4,5,7,9,10
        case ScaleMode::MajorPentatonic: mask = 0b001010010101; break; // 0,2,4,7,9
        case ScaleMode::MinorPentatonic: mask = 0b010010101001; break; // 0,3,5,7,10
        case ScaleMode::Blues:           mask = 0b010011101001; break; // 0,3,5,6,7,10
        default:                         return midiNote;
    }

    int rel = (static_cast<int>(midiNote) - static_cast<int>(rootKey_)) % 12;
    if (rel < 0) rel += 12;

    if ((mask >> rel) & 1) {
        return midiNote; // Already in scale
    }

    // Snap down to nearest in-scale degree (or up if at boundary)
    for (int offset = 1; offset <= 6; ++offset) {
        int downDeg = (rel - offset + 12) % 12;
        if ((mask >> downDeg) & 1) {
            int quantized = static_cast<int>(midiNote) - offset;
            return static_cast<uint8_t>(std::clamp(quantized, 0, 127));
        }
        int upDeg = (rel + offset) % 12;
        if ((mask >> upDeg) & 1) {
            int quantized = static_cast<int>(midiNote) + offset;
            return static_cast<uint8_t>(std::clamp(quantized, 0, 127));
        }
    }
    return midiNote;
}

void SynthEngine::renderVoices(const uint8_t* indices, size_t count, size_t frames, float* acc) {
    // Each voice renders the whole chunk in one call; vibChunk_ carries per-sample bend + vibrato,
    // which the voice samples at its control-rate ticks. Voices are summed in index order.
    for (size_t k = 0; k < count; ++k) {
        voices_[indices[k]].renderBlock(acc, static_cast<int>(frames), vibChunk_);
    }
}

void SynthEngine::renderWorkerShare() {
    renderVoices(workerVoices_, workerVoiceCount_, workerFrames_, mixWorker_);
}

void SynthEngine::renderInternal(float* outL, float* outR, size_t numFrames) {
    // Note: Do NOT lock engineMutex_ in the real-time audio thread to prevent DMA underruns
    // Deferred mode: apply queued note events / parameter pushes on the audio thread
    if (deferredEvents_) drainEvents();

    const float dtLfo = lfoRateHz_ / sampleRate_;
    const float dtTremolo = tremoloRateHz_ / sampleRate_;

    for (size_t base = 0; base < numFrames; base += RENDER_CHUNK) {
        const size_t n = std::min(RENDER_CHUNK, numFrames - base);

        // Fast LUT for mod wheel vibrato (SineLut replaces expensive std::sin in audio loop)
        const float effVibrato = std::max(modWheel_, vibratoDepth_);
        for (size_t i = 0; i < n; ++i) {
            const float lfoVal = SineLut::lookupFast(lfoPhase_ * 6.28318530718f);
            lfoPhase_ += dtLfo;
            if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;
            vibChunk_[i] = pitchBendSemitones_ + (lfoVal * effVibrato * 0.6f);
        }

        // Sum every sounding voice: allocation respects the mode / voice limit, and voices left over
        // from a mode or preset change must still finish their fade-out
        uint8_t active[MAX_VOICES];
        size_t activeCount = 0;
        for (size_t v = 0; v < MAX_VOICES; ++v) {
            if (voices_[v].isActive()) active[activeCount++] = static_cast<uint8_t>(v);
        }
        std::fill(mixMain_, mixMain_ + n, 0.0f);

        const bool useWorker = voiceWorker_ && activeCount >= MIN_VOICES_FOR_WORKER;
        if (useWorker) {
            // The worker gets the larger half: the audio thread also runs the chorus and output stage
            workerVoiceCount_ = (activeCount + 1) / 2;
            const size_t mainCount = activeCount - workerVoiceCount_;
            for (size_t k = 0; k < workerVoiceCount_; ++k) workerVoices_[k] = active[mainCount + k];
            workerFrames_ = n;
            std::fill(mixWorker_, mixWorker_ + n, 0.0f);
            voiceWorker_->start();
            renderVoices(active, mainCount, n, mixMain_);
            if (voiceWorker_->wait()) {
                for (size_t i = 0; i < n; ++i) mixMain_[i] += mixWorker_[i];
            }
        } else {
            renderVoices(active, activeCount, n, mixMain_);
        }

        for (size_t i = 0; i < n; ++i) {
            const float mix = mixMain_[i];

            // Stereo pan tremolo modulation (auto-pan, especially for FM Rhodes / E-Piano)
            float effectiveTremolo = tremoloDepth_ + (modWheel_ * 0.4f);
            if (effectiveTremolo > 0.85f) effectiveTremolo = 0.85f;

            if (effectiveTremolo > 0.01f) {
                float tremWave = SineLut::lookupFast(tremoloPhase_ * 6.28318530718f);
                float gainL = 1.0f - (effectiveTremolo * (0.5f + 0.5f * tremWave));
                float gainR = 1.0f - (effectiveTremolo * (0.5f - 0.5f * tremWave));
                tremoloPhase_ += dtTremolo;
                if (tremoloPhase_ >= 1.0f) tremoloPhase_ -= 1.0f;

                outL[base + i] = mix * gainL;
                outR[base + i] = mix * gainR;
            } else {
                outL[base + i] = mix;
                outR[base + i] = mix;
            }
        }
    }

    // Process through stereo chorus (e.g. Juno-60 lush stereo chorus)
    chorus_.processBlock(outL, outR, numFrames);

    // Mix dedicated DSP Metronome Click (dry, zero voice stealing)
    const uint32_t samplesPerBeat = metroSamplesPerBeat();
    const uint32_t clockTimeoutSamples = static_cast<uint32_t>(sampleRate_ * 1.5f);

    for (size_t i = 0; i < numFrames; ++i) {
        if (externalClockActive_) {
            samplesSinceLastClock_++;
            if (samplesSinceLastClock_ > clockTimeoutSamples) {
                externalClockActive_ = false;
            }
        }

        if (metronomeEnabled_ && !externalClockActive_) {
            metroSampleCounter_++;
            if (metroSampleCounter_ >= samplesPerBeat) {
                metroSampleCounter_ = 0;
                currentBeatIndex_ = static_cast<uint8_t>((currentBeatIndex_ + 1) % timeSigNumerator_);
                triggerMetroClick(currentBeatIndex_);
            }
        }

        if (metroClickEnv_ > 0.0005f) {
            float instFreq = metroBaseFreq_ * (1.0f + metroPitchEnv_);
            float clickSample = SineLut::lookupFast(metroPhase_ * 6.28318530718f) * metroClickEnv_ * metronomeVolume_ * kMetroClickGain;
            metroPhase_ += instFreq / sampleRate_;
            if (metroPhase_ >= 1.0f) metroPhase_ -= 1.0f;

            metroClickEnv_ *= metroClickDecay_;
            metroPitchEnv_ *= metroPitchDecay_;

            const float duck = 1.0f - kMetroDuckDepth * metroClickEnv_ * metronomeVolume_;
            outL[i] = outL[i] * duck + clickSample;
            outR[i] = outR[i] * duck + clickSample;
        }
    }
}

void SynthEngine::processBlock(int16_t* outStereoInterleaved, size_t numFrames) {
    const size_t maxChunk = tempL_.size();
    size_t offset = 0;
    while (offset < numFrames) {
        const size_t chunk = std::min(numFrames - offset, maxChunk);
        renderInternal(tempL_.data(), tempR_.data(), chunk);

        // Master saturation & conversion to 16-bit PCM stereo with smooth de-zippering
        for (size_t i = 0; i < chunk; ++i) {
            currentGain_ += (outputGain_ - currentGain_) * 0.005f;
            float outL = ParameterScaler::outputLimiter(tempL_[i] * currentGain_ * kMasterBusTrim);
            float outR = ParameterScaler::outputLimiter(tempR_[i] * currentGain_ * kMasterBusTrim);
            if (monoOutput_) {
                outL = 0.5f * (outL + outR);
                outR = 0.0f;
            }

            int16_t sL = static_cast<int16_t>(std::clamp(outL * 30000.0f, -32767.0f, 32767.0f));
            int16_t sR = static_cast<int16_t>(std::clamp(outR * 30000.0f, -32767.0f, 32767.0f));

            outStereoInterleaved[2 * (offset + i)]     = sL;
            outStereoInterleaved[2 * (offset + i) + 1] = sR;
        }
        offset += chunk;
    }
}

void SynthEngine::processBlockFloat(float* outLeft, float* outRight, size_t numFrames) {
    renderInternal(outLeft, outRight, numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        currentGain_ += (outputGain_ - currentGain_) * 0.005f;
        outLeft[i]  = ParameterScaler::outputLimiter(outLeft[i] * currentGain_ * kMasterBusTrim);
        outRight[i] = ParameterScaler::outputLimiter(outRight[i] * currentGain_ * kMasterBusTrim);
        if (monoOutput_) {
            outLeft[i]  = 0.5f * (outLeft[i] + outRight[i]);
            outRight[i] = 0.0f;
        }
    }
}

bool SynthEngine::isChorusEnabled() const {
    return chorus_.isEnabled();
}

float SynthEngine::getChorusMix() const {
    return chorus_.getMix();
}

void SynthEngine::setChorusRateHz(float rateHz) {
    chorus_.setRateHz(rateHz);
}

float SynthEngine::getChorusRateHz() const {
    return chorus_.getRateHz();
}

void SynthEngine::setChorusDepthMs(float depthMs) {
    chorus_.setDepthMs(depthMs);
}

float SynthEngine::getChorusDepthMs() const {
    return chorus_.getDepthMs();
}

void SynthEngine::setStorageCallbacks(StorageWriteCallback writeCb, StorageReadCallback readCb,
                                      V1StorageReadCallback v1ReadCb) {
    storageWriteCb_   = writeCb;
    storageReadCb_    = readCb;
    storageV1ReadCb_  = v1ReadCb;
}

bool SynthEngine::saveUserPresetToStorage(uint8_t slot) {
    if (!isUserSlot(slot)) return false;
    size_t uIdx = slotToUserIndex(slot);
    uint8_t body[MAX_PATCH_BODY_SIZE];
    const size_t len = SysExProtocol::encodePatchBody(userPresets_[uIdx], body, sizeof(body));
    if (len == 0) return false;

    bool writeOk = true;
    if (storageWriteCb_) {
        writeOk = storageWriteCb_(slot, body, len);
    }

    if (writeOk) {
        userStorageMemory_[uIdx].assign(body, body + len);
    }
    return writeOk;
}

// Restores a user slot to its built-in default patch (keeping the slot's name storage)
void SynthEngine::resetUserPreset(size_t uIdx) {
    userPresets_[uIdx] = getPresets()[FACTORY_PRESET_COUNT + uIdx];
    userPresets_[uIdx].name = userPresetNames_[uIdx].data();
}

bool SynthEngine::loadUserPresetFromStorage(uint8_t slot) {
    if (!isUserSlot(slot)) return false;
    size_t uIdx = slotToUserIndex(slot);
    auto& preset = userPresets_[uIdx];
    char* name = userPresetNames_[uIdx].data();
    bool loaded = false;

    // v2 body first; then a legacy v1 blob (migrated to v2 on the slot's next save); then this session's save
    uint8_t body[MAX_PATCH_BODY_SIZE];
    const size_t len = storageReadCb_ ? storageReadCb_(slot, body, sizeof(body)) : 0;
    if (len > 0 && len <= sizeof(body) && SysExProtocol::isValidPatchBody(body, len)) {
        resetUserPreset(uIdx);
        loaded = SysExProtocol::decodePatchBody(body, len, preset, name);
    }
    if (!loaded && storageV1ReadCb_) {
        RawPatchData raw;
        if (storageV1ReadCb_(slot, reinterpret_cast<uint8_t*>(&raw), sizeof(raw))) {
            SysExProtocol::rawPatchToPreset(raw, preset, name);
            loaded = true;
        }
    }
    if (!loaded && !userStorageMemory_[uIdx].empty()) {
        const auto& mem = userStorageMemory_[uIdx];
        resetUserPreset(uIdx);
        loaded = SysExProtocol::decodePatchBody(mem.data(), mem.size(), preset, name);
    }

    if (loaded) {
        preset.model = static_cast<SynthModel>(FACTORY_PRESET_COUNT + uIdx);
        if (currentPresetIndex_ == FACTORY_PRESET_COUNT + uIdx) {
            applyPreset(preset);
        }
    }
    return loaded;
}

bool SynthEngine::loadAllUserPresetsFromStorage() {
    bool allLoaded = true;
    for (uint8_t s = USER_SLOT_START; s <= USER_SLOT_END; ++s) {
        if (!loadUserPresetFromStorage(s)) {
            allLoaded = false;
        }
    }
    return allLoaded;
}

Preset& SynthEngine::getUserPreset(uint8_t slot) {
    return userPresets_[slotToUserIndex(slot)];
}

const Preset& SynthEngine::getUserPreset(uint8_t slot) const {
    return userPresets_[slotToUserIndex(slot)];
}

void SynthEngine::sendSysExResponse(const uint8_t* data, size_t len) {
    lastSysExResponse_.assign(data, data + len);
    if (replyOut_) replyOut_->assign(data, data + len);
    if (onSysExSend_) {
        onSysExSend_(data, len);
    }
}

void SynthEngine::sendHandshake(SysExCommand refCmd, HandshakeStatus status) {
    uint8_t buf[7];
    const size_t len = SysExProtocol::encodeHandshake(refCmd, status, buf, sizeof(buf));
    sendSysExResponse(buf, len);
}

// Applies one patch-scope parameter (physical units, see SysExProtocol's table) to the live sound
void SynthEngine::applyLiveParam(ParamId id, float v) {
    switch (id) {
        case ParamId::Cutoff:            setCutoffHz(v); break;
        case ParamId::Resonance:         setResonanceQ(v); break;
        case ParamId::Waveform:          setWaveform(static_cast<Waveform>(static_cast<uint8_t>(v))); break;
        case ParamId::SubOscLevel:       setSubOscLevel(v); break;
        case ParamId::DetuneCents:       setDetuneCents(v); break;
        case ParamId::Drive:             setDrive(v); break;
        case ParamId::FilterEnvAmount:   setFilterEnvAmount(v); break;
        case ParamId::AmpAttack:         setAmpAttackMs(v); break;
        case ParamId::AmpDecay:          setAmpDecayMs(v); break;
        case ParamId::AmpSustain:        setAmpSustainLevel(v); break;
        case ParamId::AmpRelease:        setAmpReleaseMs(v); break;
        case ParamId::FilterAttack:      setFilterAttackMs(v); break;
        case ParamId::FilterDecay:       setFilterDecayMs(v); break;
        case ParamId::FilterSustain:     setFilterSustainLevel(v); break;
        case ParamId::FilterRelease:     setFilterReleaseMs(v); break;
        case ParamId::ChorusEnabled:     setChorusEnabled(v >= 0.5f); break;
        case ParamId::ChorusRate:        chorus_.setRateHz(v); break;
        case ParamId::ChorusDepth:       chorus_.setDepthMs(v); break;
        case ParamId::ChorusMix:         setChorusMix(v); break;
        case ParamId::VoiceMode:         setVoiceMode(static_cast<VoiceMode>(static_cast<uint8_t>(v))); break;
        case ParamId::GlideTime:         setGlideTimeMs(v); break;
        case ParamId::PortamentoEnabled: setPortamentoEnabled(v >= 0.5f); break;
        case ParamId::LegatoEnabled:     setLegatoEnabled(v >= 0.5f); break;
        case ParamId::ModRatio:          setModRatio(v); break;
        case ParamId::ModDepth:          setModDepth(v); break;
        case ParamId::ModFeedback:       setModFeedback(v); break;
        case ParamId::TremoloDepth:      setTremoloDepth(v); break;
        case ParamId::TremoloRate:       setTremoloRateHz(v); break;
        case ParamId::EngineMode:
            setEngineModeAndVoicing(static_cast<EngineMode>(std::min<uint8_t>(static_cast<uint8_t>(v),
                                        static_cast<uint8_t>(EngineMode::Count) - 1)), patchVoicing_);
            updateAllVoices();
            break;
        case ParamId::WtTable:           setWavetable(static_cast<uint8_t>(v)); break;
        case ParamId::WtPosition:        setWavetablePosition(v); break;
        case ParamId::WtEnvAmount:       setWavetableEnvAmount(v); break;
        case ParamId::PdWave:            setPdWave(static_cast<PdWave>(static_cast<uint8_t>(v))); break;
        case ParamId::PdDcw:             setPdDcw(v); break;
        case ParamId::PluckDecay:        setPluckDecayMs(v); break;
        case ParamId::PluckBright:       setPluckBrightness(v); break;
        case ParamId::PluckPick:         setPluckPickPosition(v); break;
        case ParamId::PercLevel:         setPercussionLevel(v); break;
        case ParamId::PercHarmonic:      setPercussionHarmonic(static_cast<uint8_t>(v)); break;
        case ParamId::KeyClick:          setKeyClick(v); break;
        case ParamId::Voicing: {
            const uint8_t idx = static_cast<uint8_t>(v);
            setEngineModeAndVoicing(engineMode_, idx == 0 ? SynthModel::Count : static_cast<SynthModel>(idx - 1));
            updateAllVoices();
            break;
        }
        default:
            if (id >= ParamId::Drawbar1 && id <= ParamId::Drawbar9) {
                setDrawbar(static_cast<size_t>(static_cast<uint8_t>(id) - static_cast<uint8_t>(ParamId::Drawbar1)), v);
            }
            break;
    }
}

void SynthEngine::handleSysEx(const uint8_t* data, size_t len, std::vector<uint8_t>& reply) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    reply.clear();
    replyOut_ = &reply;
    handleSysEx(data, len);
    replyOut_ = nullptr;
}

// The dump buffer lives on the heap: handleSysEx also runs on the 4 KB USB client task for every MK4 SysEx
void SynthEngine::replyWithPatchDump(uint8_t slot, const Preset& preset) {
    std::vector<uint8_t> dumpBuf(MAX_SYSEX_MESSAGE_SIZE);
    const size_t dumpLen = SysExProtocol::encodePatchDumpV2(slot, preset, dumpBuf.data(), dumpBuf.size());
    sendSysExResponse(dumpBuf.data(), dumpLen);
}

void SynthEngine::handleSysEx(const uint8_t* data, size_t len) {
    std::lock_guard<EngineRecursiveMutex> lock(engineMutex_);
    DecodedSysEx decoded = SysExProtocol::decodeSysEx(data, len);
    if (!decoded.isValid) {
        // Send NACK response if addressed to mk4-synth manufacturer/family
        // (Note: Command 0x01 FastParamUpdate is strictly fire-and-forget; never send NACK)
        if (len >= 4 && data[1] == SYSEX_MANUFACTURER_ID && data[2] == SYSEX_FAMILY_ID) {
            SysExCommand ref = static_cast<SysExCommand>(data[3]);
            if (ref != SysExCommand::FastParamUpdate) {
                sendHandshake(ref, decoded.errorStatus);
            }
        }
        return;
    }

    switch (decoded.type) {
        case SysExCommand::FastParamUpdate: {
            const auto& p = decoded.fastParam;
            float norm = static_cast<float>(p.value14) / 16383.0f;

            if (const ParamDef* def = SysExProtocol::findPatchParam(p.paramId)) {
                const float value = SysExProtocol::value14ToParam(*def, p.value14);
                if (isUserSlot(p.slot)) {
                    const size_t uIdx = slotToUserIndex(p.slot);
                    def->set(userPresets_[uIdx], value);
                    if (currentPresetIndex_ != FACTORY_PRESET_COUNT + uIdx) {
                        break; // Only update user preset state, leave the active preset intact
                    }
                } else if (p.slot != 0) {
                    // Read-only factory slot or invalid slot: do not modify active engine parameters
                    break;
                }
                applyLiveParam(p.paramId, value);
                break;
            }

            // Global (performance) parameters apply only when the update targets the live sound: slot 0
            // or the active user slot
            const bool targetsLive = p.slot == 0 ||
                (isUserSlot(p.slot) && currentPresetIndex_ == FACTORY_PRESET_COUNT + slotToUserIndex(p.slot));
            if (!targetsLive) break;
            switch (p.paramId) {
                case ParamId::MasterVolume:
                    setMasterVolume(norm);
                    break;
                case ParamId::MetronomeEnabled:
                    setMetronomeEnabled(norm >= 0.5f || p.value14 > 0);
                    break;
                case ParamId::MetronomeBpm:
                    if (p.value14 >= 40 && p.value14 <= 240) {
                        setMetronomeBpm(static_cast<float>(p.value14));
                    } else {
                        setMetronomeBpm(40.0f + norm * 200.0f);
                    }
                    break;
                case ParamId::TimeSigNumerator:
                    setTimeSignature(static_cast<uint8_t>(std::clamp<uint16_t>(p.value14, 1, 16)), timeSigDenominator_);
                    break;
                case ParamId::TimeSigDenominator:
                    setTimeSignature(timeSigNumerator_, static_cast<uint8_t>(p.value14));
                    break;
                case ParamId::RootKey:
                    setRootKey(static_cast<uint8_t>(p.value14 % 12));
                    break;
                case ParamId::ScaleMode:
                    setScaleMode(static_cast<ScaleMode>(p.value14 % static_cast<uint16_t>(ScaleMode::Count)));
                    break;
                default:
                    break;
            }
            break;
        }

        case SysExCommand::PatchDump: {
            // Legacy v1 (62-byte) dump
            uint8_t slot = decoded.slot;
            if (slot == 0) {
                // Live active RAM edit buffer update
                Preset dumpPreset;
                char tempName[16];
                SysExProtocol::rawPatchToPreset(decoded.patchData, dumpPreset, tempName);
                applyPreset(dumpPreset);
            } else if (isUserSlot(slot)) {
                size_t uIdx = slotToUserIndex(slot);
                SysExProtocol::rawPatchToPreset(decoded.patchData, userPresets_[uIdx], userPresetNames_[uIdx].data());
                userPresets_[uIdx].model = static_cast<SynthModel>(FACTORY_PRESET_COUNT + uIdx);
                saveUserPresetToStorage(slot);
                if (currentPresetIndex_ == FACTORY_PRESET_COUNT + uIdx) {
                    applyPreset(userPresets_[uIdx]);
                }
            } else {
                // Factory presets cannot be overwritten (or slot > 32)
                sendHandshake(SysExCommand::PatchDump, HandshakeStatus::NackInvalidSlot);
                return;
            }
            sendHandshake(SysExCommand::PatchDump, HandshakeStatus::AckSuccess);
            break;
        }

        case SysExCommand::PatchDumpV2: {
            const uint8_t slot = decoded.slot;
            const auto& body = decoded.patchBody;
            if (slot == 0) {
                // Live edit buffer: entries apply on top of the default user patch, as for a stored slot
                Preset dumpPreset = getPresets()[FACTORY_PRESET_COUNT];
                char tempName[PATCH_NAME_MAX + 1];
                SysExProtocol::decodePatchBody(body.data(), body.size(), dumpPreset, tempName);
                applyPreset(dumpPreset);
            } else if (isUserSlot(slot)) {
                const size_t uIdx = slotToUserIndex(slot);
                resetUserPreset(uIdx);
                SysExProtocol::decodePatchBody(body.data(), body.size(), userPresets_[uIdx], userPresetNames_[uIdx].data());
                userPresets_[uIdx].model = static_cast<SynthModel>(FACTORY_PRESET_COUNT + uIdx);
                const bool ok = saveUserPresetToStorage(slot);
                if (currentPresetIndex_ == FACTORY_PRESET_COUNT + uIdx) {
                    applyPreset(userPresets_[uIdx]);
                }
                if (!ok) {
                    sendHandshake(SysExCommand::PatchDumpV2, HandshakeStatus::NackCorrupted);
                    return;
                }
            } else {
                sendHandshake(SysExCommand::PatchDumpV2, HandshakeStatus::NackInvalidSlot);
                return;
            }
            sendHandshake(SysExCommand::PatchDumpV2, HandshakeStatus::AckSuccess);
            break;
        }

        case SysExCommand::SaveUserSlot: {
            uint8_t slot = decoded.targetSlot;
            if (!isUserSlot(slot)) {
                sendHandshake(SysExCommand::SaveUserSlot, HandshakeStatus::NackInvalidSlot);
                return;
            }

            size_t uIdx = slotToUserIndex(slot);
            auto& up = userPresets_[uIdx];
            up.model = static_cast<SynthModel>(FACTORY_PRESET_COUNT + uIdx);
            snapshotActiveParameters(up);

            bool ok = saveUserPresetToStorage(slot);
            sendHandshake(SysExCommand::SaveUserSlot, ok ? HandshakeStatus::AckSuccess : HandshakeStatus::NackCorrupted);
            break;
        }

        case SysExCommand::RequestDump: {
            uint8_t slot = decoded.targetSlot;
            Preset out;

            if (slot == 0) {
                // Current live parameters. A user model, so a factory FM model cannot override a live
                // engine-mode change; the voicing is carried by `voicing` (the patch's own voicing)
                out = getCurrentPreset();
                snapshotActiveParameters(out);
                out.model = SynthModel::User;
            } else if (isFactorySlot(slot)) {
                out = getPresets()[slot - 1];
            } else if (isUserSlot(slot)) {
                out = userPresets_[slotToUserIndex(slot)];
            } else {
                // Invalid slot (slot > 32)
                sendHandshake(SysExCommand::RequestDump, HandshakeStatus::NackInvalidSlot);
                return;
            }

            replyWithPatchDump(slot, out);
            break;
        }

        default:
            break;
    }
}

// =========================================================================
// Animation Context API (for OLED display)
// =========================================================================

uint32_t SynthEngine::metroSamplesPerBeat() const {
    const float beatUnitScale = (timeSigDenominator_ == 8) ? 0.5f : (4.0f / static_cast<float>(timeSigDenominator_));
    return static_cast<uint32_t>(std::max(100.0f, (60.0f / metronomeBpm_) * beatUnitScale * sampleRate_));
}

float SynthEngine::getBeatPhase() const {
    if (!metronomeEnabled_ || metronomeBpm_ <= 0.0f) return 0.0f;
    // Read the same counters that fire the click, so the phase restarts exactly on each audible beat
    if (externalClockActive_) {
        const uint8_t ticksPerBeat = (timeSigDenominator_ == 8) ? 12 : 24;
        return std::min(static_cast<float>(midiClockTickCount_) / ticksPerBeat, 0.999f);
    }
    return std::min(static_cast<float>(metroSampleCounter_) / static_cast<float>(metroSamplesPerBeat()), 0.999f);
}

void SynthEngine::getVoiceSnapshot(uint8_t outNotes[8], float outEnvelopes[8], uint8_t& outCount) const {
    outCount = 0;
    for (size_t i = 0; i < MAX_VOICES; ++i) {
        if (voices_[i].isActive()) {
            outNotes[i] = voices_[i].getNoteNumber();
            outEnvelopes[i] = voices_[i].getEnvelopeLevel();
            ++outCount;
        } else {
            outNotes[i] = 0;
            outEnvelopes[i] = 0.0f;
        }
    }
}

} // namespace Synth
