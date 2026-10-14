// Offline renderer: plays a preset through SynthEngine without audio or MIDI hardware and writes a WAV.
// Single-threaded, immediate event mode, events applied sample-accurately between render calls.

#include "SynthEngine.h"
#include "Parameters.h"
#include "Presets.h"
#include "sim_util.h"
#include "smf_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kSampleRate = static_cast<uint32_t>(Synth::SynthConfig::DEFAULT_SAMPLE_RATE);

void printUsage(std::ostream& os) {
    os << "Usage: synth_render --preset N|all [--out file.wav] [options]\n"
          "\n"
          "Renders a SynthEngine preset offline (44.1 kHz stereo) to a WAV file.\n"
          "\n"
          "  --preset N|all     Preset slot 1.." << Synth::TOTAL_PRESET_COUNT
       << " (factory 1.." << Synth::FACTORY_PRESET_COUNT << "), or 'all' for every factory preset\n"
          "  --out FILE         Output WAV (with 'all', _NN is inserted before the extension)\n"
          "  --notes SPEC       Steps separated by spaces, chords joined by commas, 'r' = rest,\n"
          "                     ':SEC' overrides a step's length. Default \"C4,E4,G4\".\n"
          "                     e.g. \"C3 E3 G3 C4,E4,G4:1.5\" or \"A2:0.25 r:0.25 A2:0.25\"\n"
          "  --dur SEC          Length of each --notes step (default 2.0)\n"
          "  --velocity V       Note velocity 1..127 for --notes (default 100)\n"
          "  --midi FILE        Play a Standard MIDI File (format 0/1: notes, CCs, pitch bend,\n"
          "                     tempo) instead of --notes\n"
          "  --tail SEC         Extra render time after the last event for releases (default 1.0)\n"
          "  --float            Write 32-bit float samples instead of 16-bit PCM\n"
          "  --check            Verify the render: fails (exit 1) on NaN/inf, samples beyond full\n"
          "                     scale, or silence. --out is optional with --check.\n"
          "  --max-spread LU    Fail (exit 1) if the loudest and quietest preset's integrated\n"
          "                     loudness differ by more than LU (with --preset all; --out optional)\n"
          "  --help             Show this help\n";
}

struct Options {
    std::string preset;
    std::string out;
    std::string notes{"C4,E4,G4"};
    std::string midiFile;
    double dur{2.0};
    double tail{1.0};
    long velocity{100};
    bool asFloat{false};
    bool check{false};
    bool notesGiven{false};
    double maxSpread{-1.0};
};

struct Stats {
    float  peak{0.0f};
    double rms{0.0};
    double loudness{-999.0};   // max momentary loudness, LUFS
    double integrated{-999.0}; // gated integrated loudness, LUFS
    size_t nonFinite{0};
    size_t beyondFullScale{0};
};

double toDb(double v) { return v > 0.0 ? 20.0 * std::log10(v) : -999.0; }

void dispatch(Synth::SynthEngine& engine, const SimUtil::TimedMidi& e) {
    switch (e.status & 0xF0) {
        case 0x90:
            if (e.data2 > 0) engine.noteOn(e.data1, e.data2);
            else engine.noteOff(e.data1);
            break;
        case 0x80: engine.noteOff(e.data1); break;
        case 0xB0: engine.controlChange(e.data1, e.data2); break;
        case 0xE0: engine.pitchBend(static_cast<int16_t>(e.data1 | (e.data2 << 7))); break;
        default: break;
    }
}

// Renders preset slot `index` (0-based) playing `events`, returns interleaved stereo float samples
std::vector<float> render(size_t index, std::vector<SimUtil::TimedMidi> events, double endSec, double tailSec) {
    Synth::SynthEngine engine;
    engine.init(Synth::SynthConfig::DEFAULT_SAMPLE_RATE);
    engine.loadPresetIndex(index);

    // Time order; at equal times note-offs go first so a repeated note retriggers cleanly
    std::stable_sort(events.begin(), events.end(), [](const SimUtil::TimedMidi& a, const SimUtil::TimedMidi& b) {
        if (a.timeSec != b.timeSec) return a.timeSec < b.timeSec;
        const bool aOff = (a.status & 0xF0) == 0x80, bOff = (b.status & 0xF0) == 0x80;
        return aOff && !bOff;
    });

    const size_t totalFrames = static_cast<size_t>(std::ceil((endSec + tailSec) * kSampleRate));
    std::vector<float> out(totalFrames * 2);
    float bufL[Synth::SynthConfig::BLOCK_SIZE];
    float bufR[Synth::SynthConfig::BLOCK_SIZE];
    size_t pos = 0;

    auto renderTo = [&](size_t target) {
        target = std::min(target, totalFrames);
        while (pos < target) {
            const size_t n = std::min(target - pos, Synth::SynthConfig::BLOCK_SIZE);
            engine.processBlockFloat(bufL, bufR, n);
            for (size_t i = 0; i < n; ++i) {
                out[2 * (pos + i)]     = bufL[i];
                out[2 * (pos + i) + 1] = bufR[i];
            }
            pos += n;
        }
    };

    for (const auto& e : events) {
        renderTo(static_cast<size_t>(std::llround(std::max(0.0, e.timeSec) * kSampleRate)));
        dispatch(engine, e);
    }
    renderTo(totalFrames);
    return out;
}

Stats analyse(const std::vector<float>& samples) {
    Stats s;
    double sumSq = 0.0;
    size_t finite = 0;
    for (float x : samples) {
        if (!std::isfinite(x)) { ++s.nonFinite; continue; }
        const float a = std::fabs(x);
        s.peak = std::max(s.peak, a);
        if (a > 1.0f) ++s.beyondFullScale;
        sumSq += static_cast<double>(x) * x;
        ++finite;
    }
    s.rms = finite ? std::sqrt(sumSq / finite) : 0.0;
    s.loudness = SimUtil::maxMomentaryLoudness(samples, kSampleRate);
    s.integrated = SimUtil::integratedLoudness(samples, kSampleRate);
    return s;
}

std::string outPathFor(const std::string& base, size_t slot, bool multi) {
    if (!multi) return base;
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "_%02zu", slot);
    const size_t dot = base.find_last_of('.');
    const size_t slash = base.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return base + suffix;
    return base.substr(0, dot) + suffix + base.substr(dot);
}

int usageError(const std::string& msg) {
    std::cerr << "synth_render: " << msg << "\n\n";
    printUsage(std::cerr);
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;
        bool hasInlineValue = false;
        const size_t eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            value = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
            hasInlineValue = true;
        }
        auto takeValue = [&](std::string& dst) {
            if (hasInlineValue) { dst = value; return true; }
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };

        std::string v;
        const bool isSwitch = arg == "--help" || arg == "-h" || arg == "--float" || arg == "--check";
        if (isSwitch && hasInlineValue) return usageError(arg + " takes no value");

        if (arg == "--help" || arg == "-h") { printUsage(std::cout); return 0; }
        else if (arg == "--float")  opt.asFloat = true;
        else if (arg == "--check")  opt.check = true;
        else if (arg == "--preset") { if (!takeValue(opt.preset)) return usageError("--preset needs a value"); }
        else if (arg == "--out")    { if (!takeValue(opt.out)) return usageError("--out needs a value"); }
        else if (arg == "--midi")   { if (!takeValue(opt.midiFile)) return usageError("--midi needs a value"); }
        else if (arg == "--notes")  {
            if (!takeValue(opt.notes)) return usageError("--notes needs a value");
            opt.notesGiven = true;
        }
        else if (arg == "--dur") {
            if (!takeValue(v) || !SimUtil::parseDouble(v, opt.dur) || opt.dur <= 0.0 || opt.dur > 600.0)
                return usageError("--dur needs a length in seconds (0..600)");
        }
        else if (arg == "--tail") {
            if (!takeValue(v) || !SimUtil::parseDouble(v, opt.tail) || opt.tail < 0.0 || opt.tail > 60.0)
                return usageError("--tail needs a length in seconds (0..60)");
        }
        else if (arg == "--max-spread") {
            if (!takeValue(v) || !SimUtil::parseDouble(v, opt.maxSpread) || opt.maxSpread < 0.0)
                return usageError("--max-spread needs a loudness range in LU (>= 0)");
        }
        else if (arg == "--velocity") {
            if (!takeValue(v) || !SimUtil::parseInt(v, opt.velocity) || opt.velocity < 1 || opt.velocity > 127)
                return usageError("--velocity needs a value 1..127");
        }
        else return usageError("unknown option '" + std::string(argv[i]) + "'");
    }

    // Preset selection
    std::vector<size_t> slots;
    if (opt.preset == "all") {
        for (size_t s = 0; s < Synth::FACTORY_PRESET_COUNT; ++s) slots.push_back(s);
    } else {
        long n = 0;
        if (opt.preset.empty()) return usageError("--preset is required");
        if (!SimUtil::parseInt(opt.preset, n) || n < 1 || n > static_cast<long>(Synth::TOTAL_PRESET_COUNT))
            return usageError("--preset must be 1.." + std::to_string(Synth::TOTAL_PRESET_COUNT) + " or 'all'");
        slots.push_back(static_cast<size_t>(n - 1));
    }
    if (opt.out.empty() && !opt.check && opt.maxSpread < 0.0)
        return usageError("--out is required (unless --check or --max-spread)");
    if (!opt.midiFile.empty() && opt.notesGiven) return usageError("use either --notes or --midi, not both");
    if (opt.maxSpread >= 0.0 && slots.size() < 2) return usageError("--max-spread needs --preset all");

    // Events
    std::vector<SimUtil::TimedMidi> events;
    double endSec = 0.0;
    std::string err;
    if (!opt.midiFile.empty()) {
        if (!SimUtil::readSmfFile(opt.midiFile, events, err)) {
            std::cerr << "synth_render: " << opt.midiFile << ": " << err << "\n";
            return 1;
        }
        for (const auto& e : events) endSec = std::max(endSec, e.timeSec);
    } else if (!SimUtil::parseNoteSequence(opt.notes, opt.dur, static_cast<uint8_t>(opt.velocity), events, endSec, err)) {
        return usageError("--notes: " + err);
    }

    const auto& presets = Synth::getPresets();
    int failures = 0;
    double loudest = -999.0, quietest = 999.0;
    for (size_t slot : slots) {
        const std::vector<float> samples = render(slot, events, endSec, opt.tail);
        const Stats st = analyse(samples);

        std::string written;
        if (!opt.out.empty()) {
            written = outPathFor(opt.out, slot + 1, slots.size() > 1);
            if (!SimUtil::writeWav(written, samples, 2, kSampleRate, opt.asFloat)) {
                std::cerr << "synth_render: cannot write '" << written << "'\n";
                return 1;
            }
        }

        bool ok = true;
        std::string why;
        if (opt.check) {
            if (st.nonFinite)        { ok = false; why += " NaN/inf=" + std::to_string(st.nonFinite); }
            if (st.beyondFullScale)  { ok = false; why += " beyond-full-scale=" + std::to_string(st.beyondFullScale); }
            if (st.rms < 1.0e-4)     { ok = false; why += " silent"; } // below -80 dBFS RMS
        }
        if (!ok) ++failures;
        loudest = std::max(loudest, st.integrated);
        quietest = std::min(quietest, st.integrated);

        char line[256];
        std::snprintf(line, sizeof(line), "%s[%02zu] %-18s peak %6.1f dBFS  rms %6.1f dBFS  loud %6.1f LUFS  int %6.1f LUFS  %.2f s",
                      opt.check ? (ok ? "[PASS] " : "[FAIL] ") : "", slot + 1, presets[slot].name,
                      toDb(st.peak), toDb(st.rms), st.loudness, st.integrated, samples.size() / 2.0 / kSampleRate);
        std::cout << line << why << (written.empty() ? "" : "  -> " + written) << "\n";
    }

    if (opt.check) {
        std::cout << (failures ? "synth_render: check FAILED for " + std::to_string(failures) + " preset(s)\n"
                               : "synth_render: all checks passed\n");
    }
    if (opt.maxSpread >= 0.0) {
        const double spread = loudest - quietest;
        const bool ok = spread <= opt.maxSpread;
        char line[128];
        std::snprintf(line, sizeof(line), "synth_render: loudness spread %.1f LU (limit %.1f) %s\n",
                      spread, opt.maxSpread, ok ? "ok" : "FAILED");
        std::cout << line;
        if (!ok) ++failures;
    }
    return failures ? 1 : 0;
}
