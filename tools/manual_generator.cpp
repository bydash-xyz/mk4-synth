/**
 * manual_generator.cpp
 * 
 * Code-generates and verifies the static, printable Akai MPK Mini IV (MK4)
 * instruction manual from lib/SynthEngine/include/Presets.h.
 *
 * Produces a non-interactive, print-friendly and view-friendly manual
 * detailing how to control and modify sounds on the ESP32 synth engine.
 *
 * The HTML file holds the hand-written sections; the generator owns the regions between
 * "<!-- === BEGIN CODE-GENERATED <NAME> === -->" and the matching END marker (INTRO, MANUAL CONTENT).
 * Default values come from SynthEngine::describeParameter() on the loaded preset (same text as the MPK screen).
 * tools/generate_manual.py --pdf prints the result to a US Letter PDF with headless Chrome.
 *
 * Usage:
 *   ./manual_generator [--write <html_path>]   Update the HTML manual
 *   ./manual_generator --verify <html_path>    Verify manual matches Presets.h (used by CTest)
 */

#include "Presets.h"
#include "SynthEngine.h"
#include "Wavetables.h"
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

std::string sanitizeId(const std::string& name) {
    std::string id;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return id.empty() ? "synth" : id;
}

std::string escapeHtml(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

struct Role {
    uint8_t     cc;
    const char* text;
};

// What each parameter CC does on a subtractive preset. Ranges and defaults are not repeated here:
// the Range and Default columns come from the engine.
constexpr Role SUBTRACTIVE_ROLES[] = {
    {Synth::CC_FILTER_CUTOFF,      "Adjusts low-pass filter cutoff frequency."},
    {Synth::CC_FILTER_RESONANCE,   "Adjusts filter resonance."},
    {Synth::CC_SUB_OSC_LEVEL,      "Adjusts sub-oscillator level (one octave below the note)."},
    {Synth::CC_CHORUS_MIX,         "Adjusts stereo chorus mix. The lowest settings turn the chorus off."},
    {Synth::CC_AMP_ATTACK,         "Adjusts amplifier attack time."},
    {Synth::CC_AMP_DECAY,          "Adjusts amplifier decay time."},
    {Synth::CC_AMP_SUSTAIN,        "Adjusts amplifier sustain level."},
    {Synth::CC_AMP_RELEASE,        "Adjusts amplifier release time after key release."},
    {Synth::CC_FILTER_ENV_AMT,     "Adjusts filter envelope depth."},
    {Synth::CC_FILTER_ENV_ATTACK,  "Adjusts filter envelope attack time."},
    {Synth::CC_FILTER_ENV_DECAY,   "Adjusts filter envelope decay time."},
    {Synth::CC_FILTER_ENV_SUSTAIN, "Adjusts filter envelope sustain level."},
    {Synth::CC_FILTER_ENV_RELEASE, "Adjusts filter envelope release time."},
    {Synth::CC_CHORUS_RATE,        "Adjusts chorus LFO speed."},
    {Synth::CC_CHORUS_DEPTH,       "Adjusts chorus modulation depth."},
    {Synth::CC_DRIVE,              "Adjusts pre-filter overdrive."},
    {Synth::CC_DETUNE_DEPTH,       "Adjusts the detune of the second oscillator against the first."},
    {Synth::CC_LFO_RATE,           "Adjusts vibrato speed."},
    {Synth::CC_LFO_DEPTH,          "Adjusts vibrato depth. The modulation wheel (CC 1) adds more on top."},
    {Synth::CC_OSC_WAVEFORM,       "Selects the oscillator waveform."},
    {Synth::CC_GLIDE,              "Turns portamento on and sets its glide time (fully left = off)."},
    {Synth::CC_VOICE_MODE,         "Selects voice allocation: lower half Monophonic, upper half Polyphonic."},
    {Synth::CC_TREMOLO_DEPTH,      "Adjusts stereo pan tremolo depth."},
    {Synth::CC_TREMOLO_RATE,       "Adjusts stereo pan tremolo speed."},
    {Synth::CC_VOLUME,             "Adjusts master output volume."},
};

// The 2-operator FM presets reuse some CCs for operator parameters
constexpr Role FM_ROLES[] = {
    {Synth::CC_FILTER_CUTOFF,      "Adjusts FM modulation index: how bright the tone is."},
    {Synth::CC_FILTER_RESONANCE,   "Adjusts modulator-to-carrier frequency ratio (harmonic at whole numbers, bell-like between)."},
    {Synth::CC_SUB_OSC_LEVEL,      "Adjusts modulator self-feedback for warmth and harmonic body."},
    {Synth::CC_AMP_ATTACK,         "Adjusts carrier amplifier attack time."},
    {Synth::CC_AMP_DECAY,          "Adjusts carrier amplifier decay time."},
    {Synth::CC_AMP_SUSTAIN,        "Adjusts carrier amplifier sustain level."},
    {Synth::CC_AMP_RELEASE,        "Adjusts carrier amplifier release time after key release."},
    {Synth::CC_FILTER_ENV_AMT,     "Adjusts modulator envelope decay time (how long the strike stays bright)."},
    {Synth::CC_FILTER_ENV_ATTACK,  "Adjusts modulator envelope sustain level."},
    {Synth::CC_FILTER_ENV_DECAY,   "Adjusts modulator envelope attack time."},
    {Synth::CC_FILTER_ENV_SUSTAIN, "Adjusts stereo auto-pan tremolo depth."},
    {Synth::CC_FILTER_ENV_RELEASE, "Adjusts stereo auto-pan tremolo speed."},
};

// The Casio PT-10 presets reuse some CCs for their tone controls
constexpr Role CASIO_ROLES[] = {
    {Synth::CC_OSC_WAVEFORM,       "Selects 1 of the 4 Casio PT-10 tones."},
    {Synth::CC_FILTER_ENV_AMT,     "Adjusts oscillator pulse width."},
    {Synth::CC_FILTER_ENV_SUSTAIN, "Adjusts the level of the octave-up overtone."},
};

// The wavetable presets: the filter envelope also sweeps the wavetable position
constexpr Role WAVETABLE_ROLES[] = {
    {Synth::CC_WT_POSITION,        "Selects the start position in the wavetable (frame 1 to 8). Positions between frames blend the two."},
    {Synth::CC_WT_ENV_AMT,         "Adjusts how far the filter envelope moves the wavetable position on each note."},
    {Synth::CC_WT_TABLE,           "Selects 1 of 4 wavetables (PPG Sweep, Analog, Vox or Glass)."},
    {Synth::CC_DETUNE_DEPTH,       "Adjusts the detune of the second wavetable oscillator against the first."},
    {Synth::CC_FILTER_ENV_ATTACK,  "Adjusts filter and wave envelope attack time."},
    {Synth::CC_FILTER_ENV_DECAY,   "Adjusts filter and wave envelope decay time."},
    {Synth::CC_FILTER_ENV_SUSTAIN, "Adjusts filter and wave envelope sustain level."},
    {Synth::CC_FILTER_ENV_RELEASE, "Adjusts filter and wave envelope release time."},
};

// The Casio CZ phase distortion presets: no filter, the filter envelope drives the DCW instead
constexpr Role PHASE_DIST_ROLES[] = {
    {Synth::CC_PD_DCW,             "Adjusts the base waveform distortion (DCW). 0 gives a sine wave; higher values give more harmonics."},
    {Synth::CC_FILTER_ENV_AMT,     "Adjusts how much the DCW envelope adds to the distortion on each note."},
    {Synth::CC_PD_WAVE,            "Selects 1 of 7 CZ waveforms (Saw, Square, Pulse, Double Sine, or Resonance I, II, III)."},
    {Synth::CC_DETUNE_DEPTH,       "Adjusts the detune of the second line. At 0 only one line plays."},
    {Synth::CC_FILTER_ENV_ATTACK,  "Adjusts DCW envelope attack time."},
    {Synth::CC_FILTER_ENV_DECAY,   "Adjusts DCW envelope decay time."},
    {Synth::CC_FILTER_ENV_SUSTAIN, "Adjusts DCW envelope sustain level."},
    {Synth::CC_FILTER_ENV_RELEASE, "Adjusts DCW envelope release time."},
};

// The Karplus-Strong plucked string presets
constexpr Role PLUCK_ROLES[] = {
    {Synth::CC_PLUCK_DECAY,        "Adjusts how long the string rings (time for the note to fall by 60 dB)."},
    {Synth::CC_PLUCK_BRIGHT,       "Adjusts the brightness of the pluck and of the ringing string. Harder key strikes are also brighter."},
    {Synth::CC_PLUCK_PICK,         "Adjusts where the string is plucked: near the bridge (thin tone) to the middle (round tone)."},
    {Synth::CC_DRIVE,              "Adjusts output saturation."},
    {Synth::CC_AMP_RELEASE,        "Adjusts how fast the string stops after key release."},
};

// The Hammond additive organ presets (drawbars are described in getParamRole())
constexpr Role ADDITIVE_ROLES[] = {
    {Synth::CC_PERC_LEVEL,         "Adjusts the percussion level. Fully left turns percussion off."},
    {Synth::CC_PERC_HARMONIC,      "Selects the percussion harmonic: lower half second (4'), upper half third (2 2/3')."},
    {Synth::CC_FILTER_ENV_DECAY,   "Adjusts the percussion decay time."},
    {Synth::CC_KEY_CLICK,          "Adjusts the key click level at the start of each note."},
    {Synth::CC_TREMOLO_DEPTH,      "Adjusts the depth of the rotary speaker (stereo tremolo)."},
    {Synth::CC_TREMOLO_RATE,       "Adjusts the rotary speaker speed: about 0.8 Hz is slow (chorale), about 6.5 Hz is fast (tremolo)."},
};

struct ModelRole {
    Synth::SynthModel model;
    uint8_t           cc;
    const char*       text;
};

// Text specific to one instrument
constexpr ModelRole MODEL_ROLES[] = {
    {Synth::SynthModel::Minimoog, Synth::CC_FILTER_RESONANCE,   "Adjusts filter resonance. High values make the filter self-oscillate."},
    {Synth::SynthModel::Minimoog, Synth::CC_DRIVE,              "Adjusts transistor overdrive before the filter."},
    {Synth::SynthModel::Prophet5, Synth::CC_FILTER_RESONANCE,   "Adjusts filter resonance. High values make the filter self-oscillate."},
    {Synth::SynthModel::TB303,    Synth::CC_FILTER_RESONANCE,   "Adjusts filter resonance (the 303 squelch)."},
    {Synth::SynthModel::TB303,    Synth::CC_ACCENT,             "Adjusts accent strength. Notes played at velocity 100 or more are accented: louder, with an "
                                                                "extra filter sweep that builds up over consecutive accents."},
    {Synth::SynthModel::TB303,    Synth::CC_AMP_DECAY,          "Adjusts amplifier decay while the key is held, like the 303 VCA."},
    {Synth::SynthModel::TB303,    Synth::CC_FILTER_ENV_AMT,     "Adjusts filter envelope depth (the 303 Env Mod knob)."},
    {Synth::SynthModel::TB303,    Synth::CC_PORTAMENTO_ENABLE,  "Turns slide on (upper half) and sets its time. Overlapping notes slide without retriggering "
                                                                "the envelopes."},
    {Synth::SynthModel::CS80,     Synth::CC_DETUNE_DEPTH,       "Adjusts the detune between the two oscillator layers."},
};

template <size_t N>
const char* findRole(const Role (&roles)[N], uint8_t cc) {
    for (const auto& r : roles) {
        if (r.cc == cc) return r.text;
    }
    return nullptr;
}

// Description of one knob: model text first, then the FM / Casio family, then the subtractive default.
// A knob without a description stops generation (and fails ManualSyncTest) instead of printing filler.
std::string getParamRole(uint8_t cc, const std::string& label, const Synth::Preset& p) {
    for (const auto& r : MODEL_ROLES) {
        if (r.model == p.model && r.cc == cc) return r.text;
    }
    const Synth::EngineMode mode = Synth::presetEngineMode(p);
    if (mode == Synth::EngineMode::Additive && cc >= Synth::CC_DRAWBAR_1 && cc <= Synth::CC_DRAWBAR_9) {
        return "Sets the " + label + " drawbar from 0 (off) to 8 (full). Each step is 3 dB.";
    }
    const char* text = nullptr;
    if (Synth::isFmModel(p.model) || mode == Synth::EngineMode::FM) text = findRole(FM_ROLES, cc);
    else if (Synth::isCasioModel(p.model)) text = findRole(CASIO_ROLES, cc);
    else if (mode == Synth::EngineMode::Wavetable) text = findRole(WAVETABLE_ROLES, cc);
    else if (mode == Synth::EngineMode::PhaseDist) text = findRole(PHASE_DIST_ROLES, cc);
    else if (mode == Synth::EngineMode::Pluck) text = findRole(PLUCK_ROLES, cc);
    else if (mode == Synth::EngineMode::Additive) text = findRole(ADDITIVE_ROLES, cc);
    if (!text) text = findRole(SUBTRACTIVE_ROLES, cc);
    if (!text) {
        throw std::runtime_error(std::string("no description for knob '") + label + "' (CC " + std::to_string(cc) +
                                 ") of preset " + p.name + ": add it to the role tables in manual_generator.cpp");
    }
    std::string role = text;
    // Key tracking comes from the instrument voicing, so state it from there
    const float keyTrack = Synth::getModelVoicing(Synth::resolveVoicingModel(p)).keyTrack;
    if (cc == Synth::CC_FILTER_CUTOFF && !Synth::isFmModel(p.model) && keyTrack > 0.0f) {
        role += " The cutoff follows the keys at " + std::to_string(static_cast<int>(std::lround(keyTrack * 100.0f))) +
                " %.";
    }
    return role;
}

int slotOf(Synth::SynthModel m) {
    return static_cast<int>(m) + 1;
}

int firstUserSlot() {
    return slotOf(Synth::SynthModel::User_01);
}

int totalSlots() {
    return static_cast<int>(Synth::SynthModel::Count);
}

int factoryCount() {
    return firstUserSlot() - 1;
}

int userCount() {
    return totalSlots() - factoryCount();
}

// Factory presets in slot order, then one entry standing for all user slots
std::vector<Synth::SynthModel> displayOrder() {
    std::vector<Synth::SynthModel> order;
    for (int i = 0; i < factoryCount(); ++i) {
        order.push_back(static_cast<Synth::SynthModel>(i));
    }
    order.push_back(Synth::SynthModel::User_01);
    return order;
}

// Expands each {Label} to "Knob N (Label)" from the preset's knob layout, so instructions cannot name a knob
// the preset does not have. An unknown label stops generation (and fails ManualSyncTest).
std::string expandKnobRefs(const std::string& text, const Synth::Preset& p) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t open = text.find('{', pos);
        if (open == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        size_t close = text.find('}', open);
        if (close == std::string::npos) {
            throw std::runtime_error("unterminated knob reference in: " + text);
        }
        out += text.substr(pos, open - pos);
        std::string label = text.substr(open + 1, close - open - 1);
        int knob = 0;
        for (int k = 0; k < 8 && knob == 0; ++k) {
            if (label == p.bankA.labels[k]) knob = k + 1;
            else if (label == p.bankB.labels[k]) knob = k + 9;
        }
        if (knob == 0) {
            throw std::runtime_error(std::string("preset ") + p.name + " has no knob labelled '" + label + "'");
        }
        out += "Knob " + std::to_string(knob) + " (" + label + ")";
        pos = close + 1;
    }
    return out;
}

std::string formatRatio(float ratio) {
    std::ostringstream ss;
    if (std::fabs(ratio - std::round(ratio)) < 0.01f) ss << static_cast<int>(std::round(ratio));
    else ss << std::fixed << std::setprecision(1) << ratio;
    return ss.str() + ":1";
}

// Operating tip per model; {Label} placeholders are expanded by expandKnobRefs()
std::string getTip(Synth::SynthModel m, const Synth::Preset& p) {
    switch (m) {
        case Synth::SynthModel::Juno60:
            return "Turn {Chorus} clockwise to add stereo chorus. Keep {SubOsc} near 64 for low-end support.";
        case Synth::SynthModel::Minimoog:
            return "Turn {Drive} above 90 for saturation. Turn {Detune} to increase the beating between the oscillators.";
        case Synth::SynthModel::Prophet5:
            return "Turn {Drive} to add warm saturation. Turn {Wavefrm} to change between saw and square.";
        case Synth::SynthModel::TB303:
            return "Turn {Resonnc} above 100 for high resonance. With {Slide} in the upper half, overlapping notes slide "
                   "into each other. Play at velocity 100 or more for accented notes.";
        case Synth::SynthModel::CS80:
            return "Turn {Attack} to set the brass swell time. Push the modulation wheel forward to add vibrato.";
        case Synth::SynthModel::OBXa:
            return "Turn {FiltEnv} clockwise for a brighter filter sweep on each chord.";
        case Synth::SynthModel::CasioPT10_Piano:
            return "Turn {Cutoff} clockwise for a brighter tone. Turn {Decay} to make the key strike longer or shorter.";
        case Synth::SynthModel::CasioPT10_Fantasy:
            return "Push the modulation wheel forward or turn {VibDepth} for more vibrato. Turn {VibRate} to change the "
                   "vibrato speed.";
        case Synth::SynthModel::CasioPT10_Violin:
            return "Play one note at a time: the last note played has priority. Turn {Attack} to change the swell time.";
        case Synth::SynthModel::CasioPT10_Flute:
            return "Turn {Cutoff} counter-clockwise for a rounder flute tone. Turn {Attack} to change the breath attack.";
        case Synth::SynthModel::DX_EPiano:
            return "Strike the keys harder for a brighter tine. Turn {Feedback} for more body.";
        case Synth::SynthModel::FM_CrystalTines:
            return "The " + formatRatio(p.modRatio) + " ratio gives bell-like overtones. Turn {ModDepth} to control the "
                   "metallic ring.";
        case Synth::SynthModel::FM_WarmRhodes:
            return "Turn {Tremolo} and {TremRate} to set the depth and speed of the stereo auto-pan.";
        case Synth::SynthModel::FM_Clavinet:
            return "Play short staccato notes for a punchy plucked attack. Strike harder for a brighter tone.";
        case Synth::SynthModel::FM_DrawbarOrgan:
            return "Notes sustain without decay. Turn {ModDepth} to add upper harmonics.";
        case Synth::SynthModel::PPG_Wave:
            return "Each note sweeps through the wavetable. Turn {WaveEnv} for a longer sweep and {WavePos} to change "
                   "the start. Turn {Table} to select a different wavetable.";
        case Synth::SynthModel::WT_VoxPad:
            return "Hold a chord: the tone moves slowly from 'oo' to 'ee'. Turn {F-Attack} to change the speed of the "
                   "vowel movement.";
        case Synth::SynthModel::CZ_Bass:
            return "Turn {DCW-Env} to set the brightness of the attack and {DCW-Dec} to set how fast it closes.";
        case Synth::SynthModel::CZ_Reso:
            return "Turn {Wave} to compare the three resonance waveforms. Set {Detune} to 0 for one line only.";
        case Synth::SynthModel::KS_Guitar:
            return "Strike the keys harder for a brighter pluck. Turn {PickPos} toward 0 for a thinner tone near the "
                   "bridge.";
        case Synth::SynthModel::KS_Koto:
            return "Push the modulation wheel forward for vibrato. Turn {StrDecay} to make the notes ring longer.";
        case Synth::SynthModel::B3_Jazz:
            return "Pull the drawbars on knobs 1 to 8 and {1'} on Bank B. Turn {Perc} fully left to turn off the "
                   "percussion.";
        case Synth::SynthModel::B3_Gospel:
            return "Turn {LeslRate} counter-clockwise to slow the rotary speaker. Turn {Click} for more key click.";
        default:
            if (Synth::isUserModel(m)) {
                return "Turn the knobs to edit the patch. To keep it, send SysEx command 0x03 with a slot number (" +
                       std::to_string(firstUserSlot()) + " to " + std::to_string(totalSlots()) +
                       ") over Bluetooth LE MIDI: it saves the sound you are playing, including knob changes.";
            }
            return "Use the Bank A and Bank B knobs to change the parameters.";
    }
}

// Voices the engine allows this preset: the factory voicing caps polyphony at the original instrument's
int presetVoices(const Synth::Preset& p) {
    const auto voicing = Synth::getModelVoicing(Synth::resolveVoicingModel(p));
    const int engineMax = static_cast<int>(Synth::SynthEngine::MAX_VOICES);
    return voicing.voices > 0 ? std::min<int>(voicing.voices, engineMax) : engineMax;
}

std::string voiceModeString(Synth::SynthModel m, const Synth::Preset& p) {
    const std::string poly = "Polyphonic (" + std::to_string(presetVoices(p)) + " voices)";
    if (Synth::isUserModel(m)) return "Customizable / " + poly;
    if (p.voiceMode != Synth::VoiceMode::Monophonic) return poly;
    std::string s = "Monophonic";
    if (p.portamentoEnabled && p.glideTimeMs > 0.0f) {
        s += " (" + std::to_string(static_cast<int>(std::round(p.glideTimeMs))) + " ms " +
             (m == Synth::SynthModel::TB303 ? "slide" : "glide") + ")";
    }
    return s;
}

// Value each knob's parameter has when the preset loads, in the units the MPK screen shows (e.g. "1400 Hz").
// Uses the engine's own formatter so the manual matches the hardware display.
std::string loadedValue(Synth::SynthModel model, bool isBankB, int knob) {
    static Synth::SynthEngine engine;
    const auto& p = Synth::getPresets()[static_cast<size_t>(model)];
    const auto& bank = isBankB ? p.bankB : p.bankA;
    engine.loadPresetIndex(static_cast<size_t>(model));
    engine.setShiftActive(isBankB);
    std::string label;
    std::string value;
    if (!engine.describeParameter(bank.ccs[knob], bank.defaults[knob], label, value)) {
        value = std::to_string(static_cast<int>(bank.defaults[knob]));
    }
    engine.setShiftActive(false);
    return value;
}

// Knob range as the MPK screen shows it. Switch-like parameters (4 or fewer settings) list every setting;
// continuous ones show the values at CC 0 and CC 127. Uses its own engine because controlChange() leaves state
// (e.g. master volume) that a preset load does not reset.
std::string rangeText(Synth::SynthModel model, bool isBankB, int knob) {
    static Synth::SynthEngine engine;
    const auto& p = Synth::getPresets()[static_cast<size_t>(model)];
    const auto& bank = isBankB ? p.bankB : p.bankA;
    std::vector<std::string> settings;
    std::string first;
    std::string last;
    for (int v = 0; v <= 127; ++v) {
        engine.loadPresetIndex(static_cast<size_t>(model));
        engine.setShiftActive(isBankB);
        engine.controlChange(bank.ccs[knob], static_cast<uint8_t>(v));
        std::string label;
        std::string value;
        if (!engine.describeParameter(bank.ccs[knob], static_cast<uint8_t>(v), label, value)) value = std::to_string(v);
        engine.setShiftActive(false);
        if (v == 0) first = value;
        last = value;
        if (std::find(settings.begin(), settings.end(), value) == settings.end()) settings.push_back(value);
    }
    if (settings.size() > 4) return first + " .. " + last;
    std::string out;
    for (const auto& setting : settings) out += (out.empty() ? "" : " / ") + setting;
    return out;
}

std::string waveSvg(const char* label, const char* path) {
    return std::string("<svg viewBox=\"0 0 100 32\" class=\"wave-mini-svg\" aria-label=\"") + label +
           "\"><line x1=\"2\" y1=\"16\" x2=\"98\" y2=\"16\" class=\"wave-zero\"/><path d=\"" + path +
           "\" class=\"wave-trace\" fill=\"none\"/></svg>";
}

// Drawing of the preset's oscillator: FM sine, the Casio tone shapes, or the basic subtractive waveform
std::string waveformSvg(Synth::SynthModel model, const Synth::Preset& p) {
    switch (Synth::presetEngineMode(p)) {
        case Synth::EngineMode::FM:
            return waveSvg("FM Sine Operator", "M 4 16 Q 16 0, 28 16 T 52 16 T 76 16 T 96 16");
        case Synth::EngineMode::Wavetable:
            return waveSvg("Wavetable Frames", "M 4 16 C 10 2 14 2 18 16 C 22 30 26 30 30 16 L 34 6 L 40 26 L 46 10 L 50 16 "
                                               "C 56 4 60 10 64 16 C 68 22 72 28 76 16 L 80 8 L 84 24 L 88 12 L 96 16");
        case Synth::EngineMode::PhaseDist:
            return waveSvg("Phase-Distorted Cosine", "M 4 8 C 8 8 10 24 14 24 L 50 8 C 54 8 56 24 60 24 L 96 8");
        case Synth::EngineMode::Pluck:
            return waveSvg("Plucked String Decay", "M 4 16 L 8 2 L 12 30 L 16 5 L 20 27 L 26 8 L 32 24 L 40 11 L 48 21 "
                                                   "L 58 13 L 68 19 L 80 15 L 96 16");
        case Synth::EngineMode::Additive:
            return waveSvg("Drawbar Partials", "M 4 16 C 8 4 12 6 16 12 C 20 18 22 4 28 10 C 34 16 36 28 42 22 "
                                               "C 48 16 50 28 54 22 C 58 16 62 4 68 8 C 74 12 76 26 82 24 C 88 22 92 12 96 16");
        default:
            break;
    }
    switch (model) {
        case Synth::SynthModel::CasioPT10_Piano:
            return waveSvg("70% Pulse", "M 4 24 L 4 8 L 68 8 L 68 24 L 96 24");
        case Synth::SynthModel::CasioPT10_Fantasy:
            return waveSvg("Square + Octave", "M 4 24 L 4 8 L 26 8 L 26 14 L 50 14 L 50 24 L 74 24 L 74 18 L 96 18");
        case Synth::SynthModel::CasioPT10_Violin:
            return waveSvg("22% Multipulse", "M 4 24 L 4 8 L 16 8 L 16 24 L 34 24 L 34 8 L 46 8 L 46 24 L 64 24 L 64 8 L 76 8 L 76 24 L 96 24");
        case Synth::SynthModel::CasioPT10_Flute:
            return waveSvg("Filtered Square", "M 4 24 C 6 12 14 8 26 8 C 38 8 46 8 50 16 C 54 24 62 24 74 24 C 86 24 94 20 96 16");
        default:
            break;
    }
    if (p.waveform == Synth::Waveform::Square) {
        return waveSvg("Square", "M 4 24 L 4 8 L 26 8 L 26 24 L 50 24 L 50 8 L 72 8 L 72 24 L 96 24");
    }
    if (p.waveform == Synth::Waveform::Triangle) {
        return waveSvg("Triangle", "M 4 16 L 16 8 L 40 24 L 64 8 L 88 24 L 96 20");
    }
    return waveSvg("Sawtooth", "M 4 24 L 34 8 L 34 24 L 64 8 L 64 24 L 94 8 L 94 24");
}

std::string formatNumber(float v) {
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

std::string specChip(const std::string& label, const std::string& value) {
    return "          <div class=\"spec-chip\"><span>" + label + "</span><strong>" + escapeHtml(value) + "</strong></div>\n";
}

struct KnobRef {
    bool found{false};
    bool isBankB{false};
    int  index{0};
};

// The knob that sends `cc` in this preset, if any
KnobRef findKnob(const Synth::Preset& p, uint8_t cc) {
    for (int k = 0; k < 8; ++k) {
        if (p.bankA.ccs[k] == cc) return {true, false, k};
        if (p.bankB.ccs[k] == cc) return {true, true, k};
    }
    return {};
}

// Waveform drawing and key facts of a preset, read from its data and its model's voicing
std::string renderVoiceSpec(const Synth::Preset& p, Synth::SynthModel model) {
    const auto voicing = Synth::getModelVoicing(Synth::resolveVoicingModel(p));
    const bool fm = Synth::isFmModel(model) || p.engineMode == Synth::EngineMode::FM;

    std::string osc;
    const Synth::EngineMode mode = Synth::presetEngineMode(p);
    const auto& t = p.timbre;
    static const char* const kPdWaves[] = {"Saw", "Square", "Pulse", "Double Sine", "Resonance I", "Resonance II", "Resonance III"};
    if (fm) {
        osc = "2-Op FM Sine, " + formatRatio(p.modRatio) + " Ratio";
    } else if (mode == Synth::EngineMode::Wavetable) {
        std::string table = Synth::Wavetables::tableName(t.wtTable);
        for (size_t i = 1; i < table.size(); ++i) table[i] = static_cast<char>(std::tolower(table[i]));
        osc = "2 Wavetable Oscillators (" + table + ")";
        if (p.detuneCents > 0.0f) osc += ", " + formatNumber(p.detuneCents) + " ct Detune";
    } else if (mode == Synth::EngineMode::PhaseDist) {
        osc = std::string("CZ Phase Distortion, ") + kPdWaves[static_cast<size_t>(t.pdWave) % 7] +
              (p.detuneCents > 0.0f ? ", 2 Detuned Lines" : "");
    } else if (mode == Synth::EngineMode::Pluck) {
        osc = "Karplus-Strong String, " + formatNumber(std::round(t.pluckDecayMs / 100.0f) / 10.0f) + " s Decay";
    } else if (mode == Synth::EngineMode::Additive) {
        std::string reg;
        for (float bar : t.drawbars) reg += static_cast<char>('0' + static_cast<int>(std::lround(bar)));
        osc = "9 Drawbars " + reg + (t.percLevel > 0.0f ? (t.percHarmonic ? ", 3rd Perc" : ", 2nd Perc") : "");
    } else if (Synth::isCasioModel(model)) {
        // Casio tones are set by SynthEngine::setCasioTone(), not by preset fields
        switch (model) {
            case Synth::SynthModel::CasioPT10_Piano:   osc = "70% Asymmetric Pulse";      break;
            case Synth::SynthModel::CasioPT10_Fantasy: osc = "50% Square + Octave Layer"; break;
            case Synth::SynthModel::CasioPT10_Violin:  osc = "22% Narrow Multipulse";     break;
            default:                                   osc = "50% Square";                break;
        }
    } else {
        osc = (p.waveform == Synth::Waveform::Square) ? "Square" : (p.waveform == Synth::Waveform::Triangle) ? "Triangle" : "Saw";
        if (p.subOscLevel > 0.0f) osc += " + Sub";
        if (p.detuneCents > 0.0f) osc += ", " + formatNumber(p.detuneCents) + " ct Detune";
    }

    std::string filter = fm ? "None (FM Operators)" :
        (voicing.filter == Synth::FilterModel::Ladder24 ? "24 dB/oct Ladder Low-Pass" : "12 dB/oct State-Variable Low-Pass");
    if (mode == Synth::EngineMode::PhaseDist) filter = "None (DCW Envelope)";
    if (mode == Synth::EngineMode::Pluck)     filter = "None (String Loop Damping)";
    if (mode == Synth::EngineMode::Additive)  filter = "None (Drawbars)";

    std::string fx;
    if (p.chorusEnabled) fx = "Stereo Chorus";
    if (p.drive > 1.05f) fx += (fx.empty() ? "" : " + ") + std::string("Drive ") + formatNumber(p.drive) + "x";
    if (fm && p.tremoloDepth > 0.0f) fx += (fx.empty() ? "" : " + ") + std::string("Auto-Pan Tremolo");
    if (mode == Synth::EngineMode::Additive && p.tremoloDepth > 0.0f) fx += (fx.empty() ? "" : " + ") + std::string("Leslie Tremolo");
    if (fx.empty()) fx = "Dry";

    // Vibrato that plays without the mod wheel: the preset's vibrato depth knob is up from zero
    std::string vibrato = "Mod Wheel (CC 1)";
    const KnobRef depth = findKnob(p, Synth::CC_LFO_DEPTH);
    const KnobRef rate = findKnob(p, Synth::CC_LFO_RATE);
    if (depth.found && rate.found && (depth.isBankB ? p.bankB : p.bankA).defaults[depth.index] > 0) {
        vibrato = loadedValue(model, rate.isBankB, rate.index) + ", Always On";
    }

    const std::string velocity = voicing.accent ? "Accent at Velocity 100+" :
        (voicing.velToAmp > 0.0f ? "Velocity Sets Loudness" : "Fixed Level (Vintage)");

    std::ostringstream ss;
    ss << "        <div class=\"voice-spec\">\n";
    ss << "          <div class=\"timbre-wave-box\">" << waveformSvg(model, p) << "</div>\n";
    ss << specChip("OSCILLATOR", osc);
    ss << specChip("FILTER", filter);
    ss << specChip("EFFECTS", fx);
    ss << specChip("VIBRATO", vibrato);
    ss << specChip("VELOCITY", velocity);
    ss << "        </div>\n";
    return ss.str();
}

// Bank A and Bank B as two 4 x 2 knob decks; each dial points at the knob's default (the table lists the CCs)
std::string renderKnobDecks(const Synth::Preset& p, Synth::SynthModel model) {
    std::ostringstream ss;
    ss << "        <div class=\"bank-decks\">\n";
    for (int b = 0; b < 2; ++b) {
        const bool isBankB = (b == 1);
        const auto& bank = isBankB ? p.bankB : p.bankA;
        ss << "          <div class=\"hw-knob-deck\">\n";
        ss << "            <div class=\"deck-title\">" << (isBankB ? "Bank B &mdash; Knobs K9..K16" : "Bank A &mdash; Knobs K1..K8")
           << "</div>\n";
        ss << "            <div class=\"knob-matrix-grid\">\n";
        for (int k = 0; k < 8; ++k) {
            const float angleRad = (-135.0f + bank.defaults[k] / 127.0f * 270.0f - 90.0f) * 3.1415926535f / 180.0f;
            const int x1 = static_cast<int>(std::round(24.0f + 5.0f * std::cos(angleRad)));
            const int y1 = static_cast<int>(std::round(24.0f + 5.0f * std::sin(angleRad)));
            const int x2 = static_cast<int>(std::round(24.0f + 16.0f * std::cos(angleRad)));
            const int y2 = static_cast<int>(std::round(24.0f + 16.0f * std::sin(angleRad)));
            ss << "              <div class=\"knob-card\">\n";
            ss << "                <svg viewBox=\"0 0 48 48\" class=\"knob-dial-svg\" aria-hidden=\"true\">"
               << "<circle cx=\"24\" cy=\"24\" r=\"21\" class=\"dial-ring-track\"/>"
               << "<circle cx=\"24\" cy=\"24\" r=\"16\" class=\"dial-chassis-cap\"/>"
               << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2 << "\" class=\"dial-pointer-needle\"/>"
               << "<circle cx=\"24\" cy=\"24\" r=\"3.5\" class=\"dial-center-screw\"/></svg>\n";
            ss << "                <div class=\"knob-pname\">" << escapeHtml(bank.labels[k]) << "</div>\n";
            ss << "                <span class=\"val-badge\">" << escapeHtml(loadedValue(model, isBankB, k)) << "</span>\n";
            ss << "              </div>\n";
        }
        ss << "            </div>\n";
        ss << "          </div>\n";
    }
    ss << "        </div>\n";
    return ss.str();
}

// One 16-row table per preset: Bank A rows, then Bank B rows under a divider
std::string renderPresetTable(const Synth::Preset& p, Synth::SynthModel model) {
    std::ostringstream ss;
    ss << "        <table class=\"param-table\">\n";
    ss << "          <thead>\n";
    ss << "            <tr>\n";
    ss << "              <th style=\"width:7%;\">Knob</th>\n";
    ss << "              <th style=\"width:13%;\">Parameter</th>\n";
    ss << "              <th style=\"width:9%;\">MIDI CC</th>\n";
    ss << "              <th style=\"width:12%;\">Default</th>\n";
    ss << "              <th style=\"width:22%;\">Range</th>\n";
    ss << "              <th>Description</th>\n";
    ss << "            </tr>\n";
    ss << "          </thead>\n";
    ss << "          <tbody>\n";

    for (int b = 0; b < 2; ++b) {
        const bool isBankB = (b == 1);
        const auto& bank = isBankB ? p.bankB : p.bankA;
        ss << "            <tr class=\"bank-row\"><td colspan=\"6\">"
           << (isBankB ? "Bank B &mdash; press BANK A/B, then turn knobs 1 to 8" : "Bank A") << "</td></tr>\n";
        for (int k = 0; k < 8; ++k) {
            std::string label = bank.labels[k];
            uint8_t cc = bank.ccs[k];
            ss << "            <tr>\n";
            ss << "              <td><strong>K" << (k + 1 + (isBankB ? 8 : 0)) << "</strong></td>\n";
            ss << "              <td><span class=\"param-name\">" << escapeHtml(label) << "</span></td>\n";
            ss << "              <td><span class=\"badge-cc\">CC " << static_cast<int>(cc) << "</span></td>\n";
            ss << "              <td><span class=\"val-badge\">" << escapeHtml(loadedValue(model, isBankB, k)) << "</span></td>\n";
            ss << "              <td><span class=\"range-txt\">" << escapeHtml(rangeText(model, isBankB, k)) << "</span></td>\n";
            ss << "              <td>" << escapeHtml(getParamRole(cc, label, p)) << "</td>\n";
            ss << "            </tr>\n";
        }
    }

    ss << "          </tbody>\n";
    ss << "        </table>\n";
    return ss.str();
}

std::string presetDisplayName(Synth::SynthModel m, const Synth::Preset& p) {
    if (!Synth::isUserModel(m)) return p.name;
    return "USER 01.." + std::to_string(userCount());
}

std::string slotLabel(Synth::SynthModel m) {
    if (!Synth::isUserModel(m)) return std::to_string(slotOf(m));
    return std::to_string(firstUserSlot()) + ".." + std::to_string(totalSlots());
}

std::string anchorId(Synth::SynthModel m, const Synth::Preset& p) {
    return "model-" + (Synth::isUserModel(m) ? std::string("user") : sanitizeId(p.name));
}

// Header lead paragraph and table of contents
std::string generateIntroHtml() {
    std::ostringstream ss;
    const auto& presets = Synth::getPresets();

    ss << "      <!-- === BEGIN CODE-GENERATED INTRO === -->\n";
    ss << "      <p class=\"lead-text\">\n";
    ss << "        Instruction manual for playing and editing the ESP32-S3 polyphonic synthesizer with the Akai MPK Mini IV "
          "keyboard controller. Contains control procedures and parameter tables for all "
       << factoryCount() << " factory presets and the " << userCount() << " user slots.\n";
    ss << "      </p>\n";
    ss << "      <nav class=\"toc\" aria-label=\"Contents\">\n";
    ss << "        <div>\n";
    ss << "          <h4>Contents</h4>\n";
    ss << "          <ol>\n";
    ss << "            <li><a href=\"#section-quickstart\">Quick Start</a></li>\n";
    ss << "            <li><a href=\"#section-controls\">Control Surface Overview</a></li>\n";
    ss << "            <li><a href=\"#section-connections\">Connections, Patches and Troubleshooting</a></li>\n";
    ss << "            <li><a href=\"#all-simulations\">Preset Reference</a></li>\n";
    ss << "            <li><a href=\"#cheat-sheet\">Knob Matrix (one page)</a></li>\n";
    ss << "          </ol>\n";
    ss << "        </div>\n";
    ss << "        <div>\n";
    ss << "          <h4>Preset Slots</h4>\n";
    ss << "          <ol class=\"slot-list\">\n";
    for (Synth::SynthModel m : displayOrder()) {
        const auto& p = presets[static_cast<size_t>(m)];
        ss << "            <li><span>#" << slotLabel(m) << "</span> <a href=\"#" << anchorId(m, p) << "\">"
           << escapeHtml(presetDisplayName(m, p)) << "</a></li>\n";
    }
    ss << "          </ol>\n";
    ss << "        </div>\n";
    ss << "      </nav>\n";
    ss << "      <!-- === END CODE-GENERATED INTRO === -->\n";
    return ss.str();
}

std::string generateGeneratedManualHtml() {
    std::ostringstream ss;
    const auto& presets = Synth::getPresets();
    const auto order = displayOrder();

    ss << "    <!-- === BEGIN CODE-GENERATED MANUAL CONTENT === -->\n";
    ss << "    <!-- Auto-generated from lib/SynthEngine/include/Presets.h by tools/manual_generator -->\n";
    ss << "    <!-- DO NOT EDIT DIRECTLY. Run 'cmake --build build --target generate_manual' to regenerate. -->\n\n";

    // -------------------------------------------------------------
    // Section 4: Preset reference (slot, description, knobs and their defaults)
    // -------------------------------------------------------------
    ss << "    <section class=\"manual-section page-break\" id=\"all-simulations\">\n";
    ss << "      <div class=\"section-header\">\n";
    ss << "        <div class=\"section-badge\">Section 4 &bull; Preset Reference</div>\n";
    ss << "        <h2>Preset Reference</h2>\n";
    ss << "        <p class=\"section-summary\">The " << factoryCount() << " factory presets and the " << userCount()
       << " user slots (slots " << firstUserSlot() << " to " << totalSlots() << "). <strong>Default</strong> is the "
       << "value when the preset loads, as the MPK screen shows it.</p>\n";
    ss << "      </div>\n\n";

    for (Synth::SynthModel model : order) {
        const auto& p = presets[static_cast<size_t>(model)];
        const bool isUser = Synth::isUserModel(model);
        std::string slotPillStr = isUser ? "SLOTS #" + std::to_string(firstUserSlot()) + "..#" + std::to_string(totalSlots())
                                         : "SLOT #" + std::to_string(slotOf(model));
        std::string headingStr  = isUser ? presetDisplayName(model, p) + " (USER SLOTS)" : p.name;
        std::string descStr     = isUser ?
            std::to_string(userCount()) + " user presets stored in flash memory (slots " + std::to_string(firstUserSlot()) +
            " to " + std::to_string(totalSlots()) + "). Save the sound you are playing with SysEx command 0x03, or send a patch with tools/patch_sender.html "
            "(command 0x05), over Bluetooth LE MIDI. The table shows the layout of an empty user slot." :
            p.description;

        ss << "      <div class=\"model-block\" id=\"" << anchorId(model, p) << "\">\n";
        ss << "        <div class=\"model-title-row\">\n";
        ss << "          <div>\n";
        ss << "            <span class=\"slot-pill\">" << escapeHtml(slotPillStr) << "</span>\n";
        ss << "            <span class=\"model-heading\">" << escapeHtml(headingStr) << "</span>\n";
        ss << "          </div>\n";
        ss << "          <span class=\"voice-badge\">" << escapeHtml(voiceModeString(model, p)) << "</span>\n";
        ss << "        </div>\n";
        ss << "        <p class=\"model-desc\">" << escapeHtml(descStr) << "</p>\n";
        ss << "        <div class=\"tip-callout\"><strong>Tip:</strong> "
           << escapeHtml(expandKnobRefs(getTip(model, p), p)) << "</div>\n";
        ss << renderVoiceSpec(p, model);
        ss << renderKnobDecks(p, model);
        ss << renderPresetTable(p, model);
        ss << "      </div>\n\n";
    }

    ss << "    </section>\n\n";

    // -------------------------------------------------------------
    // Section 5: One-page knob matrix (landscape in print)
    // -------------------------------------------------------------
    ss << "    <section class=\"manual-section page-break matrix-section\" id=\"cheat-sheet\">\n";
    ss << "      <div class=\"section-header\">\n";
    ss << "        <div class=\"section-badge\">Section 5 &bull; Knob Matrix</div>\n";
    ss << "        <h2>Knob Matrix (Printable Cheat Sheet)</h2>\n";
    ss << "        <p class=\"section-summary\">Parameter and default value of every knob in all " << totalSlots()
       << " slots. This page prints in landscape.</p>\n";
    ss << "      </div>\n\n";

    ss << "      <table class=\"matrix-table\">\n";
    ss << "        <colgroup><col style=\"width:4.5%;\"><col style=\"width:9%;\"><col style=\"width:4%;\">";
    for (int k = 0; k < 16; ++k) ss << "<col>";
    ss << "</colgroup>\n";
    ss << "        <thead>\n";
    ss << "          <tr>\n";
    ss << "            <th rowspan=\"2\">Slot</th>\n";
    ss << "            <th rowspan=\"2\">Preset</th>\n";
    ss << "            <th rowspan=\"2\">Mode</th>\n";
    ss << "            <th colspan=\"8\">Bank A</th>\n";
    ss << "            <th colspan=\"8\" class=\"bank-b\">Bank B (press BANK A/B)</th>\n";
    ss << "          </tr>\n";
    ss << "          <tr>\n";
    for (int k = 1; k <= 16; ++k) {
        ss << "            <th" << (k > 8 ? " class=\"bank-b\"" : "") << ">K" << k << "</th>\n";
    }
    ss << "          </tr>\n";
    ss << "        </thead>\n";
    ss << "        <tbody>\n";

    for (Synth::SynthModel model : order) {
        const auto& p = presets[static_cast<size_t>(model)];
        std::string modeStr = (p.voiceMode == Synth::VoiceMode::Monophonic) ? "Mono" : "Poly";

        ss << "          <tr>\n";
        ss << "            <td><strong>" << slotLabel(model) << "</strong></td>\n";
        ss << "            <td class=\"matrix-name\">" << escapeHtml(presetDisplayName(model, p)) << "</td>\n";
        ss << "            <td>" << modeStr << "</td>\n";
        for (int k = 0; k < 16; ++k) {
            const bool isBankB = (k >= 8);
            const auto& bank = isBankB ? p.bankB : p.bankA;
            ss << "            <td" << (isBankB ? " class=\"bank-b\"" : "") << ">" << escapeHtml(bank.labels[k % 8])
               << "<span class=\"cc\">" << escapeHtml(loadedValue(model, isBankB, k % 8)) << "</span></td>\n";
        }
        ss << "          </tr>\n";
    }

    ss << "        </tbody>\n";
    ss << "      </table>\n";
    ss << "      <p class=\"matrix-note\">Small text is the value when the preset loads. Section 4 lists the MIDI CC "
          "and range of each knob. Mode is the preset default; the Casio PT-10 PolyMode knob can change it.</p>\n";
    ss << "    </section>\n\n";

    ss << "    <!-- === END CODE-GENERATED MANUAL CONTENT === -->\n";
    return ss.str();
}

struct Region {
    const char* name;
    std::string html;
};

// Locates "<!-- === BEGIN CODE-GENERATED <name> === -->" .. END as whole lines; npos when absent
bool findRegion(const std::string& content, const std::string& name, size_t& beginPos, size_t& endPos) {
    const std::string beginTag = "<!-- === BEGIN CODE-GENERATED " + name + " === -->";
    const std::string endTag = "<!-- === END CODE-GENERATED " + name + " === -->";
    beginPos = content.find(beginTag);
    endPos = content.find(endTag);
    if (beginPos == std::string::npos || endPos == std::string::npos || endPos < beginPos) return false;
    size_t lineStart = content.rfind('\n', beginPos);
    beginPos = (lineStart != std::string::npos) ? (lineStart + 1) : 0;
    size_t lineEnd = content.find('\n', endPos);
    endPos = (lineEnd != std::string::npos) ? (lineEnd + 1) : content.length();
    return true;
}

std::string normalize(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c != '\r') out.push_back(c);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

void reportMismatch(const std::string& name, const std::string& current, const std::string& expected) {
    std::cerr << "Region " << name << ": current length " << current.length() << ", expected length "
              << expected.length() << "\n";
    size_t minLen = std::min(current.length(), expected.length());
    for (size_t d = 0; d <= minLen; ++d) {
        if (d == minLen || current[d] != expected[d]) {
            size_t startSnippet = (d > 30) ? (d - 30) : 0;
            std::cerr << "First difference at char " << d << ":\n";
            std::cerr << "  Found:    ..." << current.substr(startSnippet, 60) << "...\n";
            std::cerr << "  Expected: ..." << expected.substr(startSnippet, 60) << "...\n";
            break;
        }
    }
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::string mode = "write";
    std::string targetPath = "docs/mpk_mini_mk4_manual.html";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verify" || arg == "-v") {
            mode = "verify";
            if (i + 1 < argc) targetPath = argv[++i];
        } else if (arg == "--write" || arg == "-w") {
            mode = "write";
            if (i + 1 < argc) targetPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: manual_generator [--write <html_path>] [--verify <html_path>]\n";
            return 0;
        } else {
            targetPath = arg;
        }
    }

    std::ifstream inFile(targetPath);
    if (!inFile.is_open()) {
        std::cerr << "[ManualGenerator] Error: Cannot open file: " << targetPath << "\n";
        return 1;
    }
    std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
    inFile.close();

    std::vector<Region> regions;
    try {
        regions.push_back({"INTRO", generateIntroHtml()});
        regions.push_back({"MANUAL CONTENT", generateGeneratedManualHtml()});
    } catch (const std::exception& e) {
        std::cerr << "[ManualGenerator] Error: " << e.what() << "\n";
        return 1;
    }

    bool inSync = true;
    for (const auto& region : regions) {
        size_t beginPos = 0;
        size_t endPos = 0;
        if (!findRegion(content, region.name, beginPos, endPos)) {
            std::cerr << "[ManualGenerator] Error: Missing CODE-GENERATED " << region.name << " markers in: "
                      << targetPath << "\n";
            return 1;
        }
        if (mode == "verify") {
            std::string current = normalize(content.substr(beginPos, endPos - beginPos));
            std::string expected = normalize(region.html);
            if (current != expected) {
                if (inSync) {
                    std::cerr << "\n======================================================================\n";
                    std::cerr << "[ManualSyncTest] FAIL: Akai MPK Mini MK4 manual is OUT OF SYNC with Presets.h!\n";
                }
                reportMismatch(region.name, current, expected);
                inSync = false;
            }
        } else {
            content = content.substr(0, beginPos) + region.html + content.substr(endPos);
        }
    }

    if (mode == "verify") {
        if (inSync) {
            std::cout << "[ManualSyncTest] PASS: " << targetPath << " is 100% in sync with Presets.h ("
                      << Synth::getPresets().size() << " models verified)\n";
            return 0;
        }
        std::cerr << "\nTo resolve, run:\n";
        std::cerr << "   cmake --build build --target generate_manual\n";
        std::cerr << "or:\n";
        std::cerr << "   ./build/manual_generator --write " << targetPath << "\n";
        std::cerr << "======================================================================\n\n";
        return 1;
    }

    std::ofstream outFile(targetPath);
    if (!outFile.is_open()) {
        std::cerr << "[ManualGenerator] Error: Cannot write to: " << targetPath << "\n";
        return 1;
    }
    outFile << content;
    outFile.close();

    std::cout << "[ManualGenerator] Successfully updated " << targetPath << " with "
              << Synth::getPresets().size() << " synth models and the knob matrix from Presets.h!\n";
    return 0;
}
