#pragma once

// Small helpers shared by the desktop simulator (synth_sim) and the offline renderer (synth_render):
// note-name parsing/formatting, the --notes sequence spec, and a WAV writer. Header-only, no dependencies.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace SimUtil {

// One MIDI channel message at an absolute time (seconds from the start of the render)
struct TimedMidi {
    double  timeSec;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
};

inline bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); }
    );
    return it != haystack.end();
}

// Strict integer / float parsing: the whole string must be consumed
inline bool parseInt(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

inline bool parseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// "C4" = 60 (MIDI middle C), sharps '#' and flats 'b', octaves -1..9, or a plain MIDI number "60"
inline bool parseNoteName(const std::string& text, int& outNote) {
    if (text.empty()) return false;
    long num = 0;
    if (std::isdigit(static_cast<unsigned char>(text[0]))) {
        if (!parseInt(text, num) || num < 0 || num > 127) return false;
        outNote = static_cast<int>(num);
        return true;
    }

    static const int kSemitone[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (letter < 'A' || letter > 'G') return false;
    int semitone = kSemitone[letter - 'A'];

    size_t pos = 1;
    if (pos < text.size() && text[pos] == '#') { ++semitone; ++pos; }
    else if (pos < text.size() && text[pos] == 'b') { --semitone; ++pos; }

    if (!parseInt(text.substr(pos), num) || num < -1 || num > 9) return false;
    const long note = (num + 1) * 12 + semitone;
    if (note < 0 || note > 127) return false;
    outNote = static_cast<int>(note);
    return true;
}

inline std::string noteName(int note) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (note < 0 || note > 127) return "?";
    return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

// --notes spec: whitespace-separated steps played one after another, each lasting stepSec.
// A step is a comma-separated chord ("C4,E4,G4"), "r" for a rest, and may override its
// length in seconds with a ":<sec>" suffix ("C4,E4,G4:1.5", "r:0.25").
// Appends note on/off events to `out`; `outEndSec` is when the last step ends.
inline bool parseNoteSequence(const std::string& spec, double stepSec, uint8_t velocity,
                              std::vector<TimedMidi>& out, double& outEndSec, std::string& err) {
    double t = 0.0;
    size_t steps = 0;
    size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && std::isspace(static_cast<unsigned char>(spec[i]))) ++i;
        if (i >= spec.size()) break;
        size_t j = i;
        while (j < spec.size() && !std::isspace(static_cast<unsigned char>(spec[j]))) ++j;
        std::string step = spec.substr(i, j - i);
        i = j;

        double len = stepSec;
        const size_t colon = step.find(':');
        if (colon != std::string::npos) {
            if (!parseDouble(step.substr(colon + 1), len) || len <= 0.0) {
                err = "bad step length in '" + step + "'";
                return false;
            }
            step = step.substr(0, colon);
        }

        if (step != "r" && step != "R") {
            size_t k = 0;
            while (k <= step.size()) {
                size_t comma = step.find(',', k);
                if (comma == std::string::npos) comma = step.size();
                int note = 0;
                const std::string name = step.substr(k, comma - k);
                if (!parseNoteName(name, note)) {
                    err = "bad note name '" + name + "' (expected e.g. C4, F#3, Bb2 or a MIDI number)";
                    return false;
                }
                out.push_back({t, 0x90, static_cast<uint8_t>(note), velocity});
                out.push_back({t + len, 0x80, static_cast<uint8_t>(note), 0});
                k = comma + 1;
            }
        }
        t += len;
        ++steps;
    }
    if (steps == 0) {
        err = "empty note spec";
        return false;
    }
    outEndSec = t;
    return true;
}

// Writes interleaved samples as a RIFF/WAVE file: 16-bit PCM (full scale 1.0 = 32767, clamped)
// or 32-bit IEEE float (samples written unmodified)
inline bool writeWav(const std::string& path, const std::vector<float>& interleaved,
                     uint16_t channels, uint32_t sampleRate, bool asFloat) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    auto put16 = [f](uint16_t v) { const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    auto put32 = [f](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        std::fwrite(b, 1, 4, f);
    };

    const uint16_t bytesPerSample = asFloat ? 4 : 2;
    const uint32_t frames   = static_cast<uint32_t>(interleaved.size() / channels);
    const uint32_t dataSize = static_cast<uint32_t>(interleaved.size()) * bytesPerSample;
    const uint32_t fmtSize  = asFloat ? 18 : 16;                 // non-PCM formats carry cbSize
    const uint32_t factSize = asFloat ? 12 : 0;                  // non-PCM formats need a 'fact' chunk
    const uint32_t riffSize = 4 + (8 + fmtSize) + factSize + (8 + dataSize);

    std::fwrite("RIFF", 1, 4, f); put32(riffSize);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); put32(fmtSize);
    put16(asFloat ? 3 : 1);                                      // WAVE_FORMAT_IEEE_FLOAT / PCM
    put16(channels);
    put32(sampleRate);
    put32(sampleRate * channels * bytesPerSample);
    put16(static_cast<uint16_t>(channels * bytesPerSample));
    put16(static_cast<uint16_t>(bytesPerSample * 8));
    if (asFloat) {
        put16(0);
        std::fwrite("fact", 1, 4, f); put32(4); put32(frames);
    }
    std::fwrite("data", 1, 4, f); put32(dataSize);

    for (float s : interleaved) {
        if (asFloat) {
            uint32_t bits;
            static_assert(sizeof(bits) == sizeof(s), "float must be 32-bit");
            std::memcpy(&bits, &s, 4);
            put32(bits);
        } else {
            const float c = std::isfinite(s) ? std::clamp(s, -1.0f, 1.0f) : 0.0f;
            put16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(c * 32767.0f))));
        }
    }
    const bool ok = std::ferror(f) == 0;
    return (std::fclose(f) == 0) && ok;
}

// ITU-R BS.1770 mean K-weighted power (both channels summed) of each 400 ms block, blocks every 100 ms.
// Empty if the input is shorter than one block.
inline std::vector<double> loudnessBlockPowers(const std::vector<float>& interleaved, uint32_t sampleRate) {
    struct Biquad {
        double b0, b1, b2, a1, a2, z1{0.0}, z2{0.0};
        double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    // K-weighting: head-related high shelf, then the RLB high-pass. Bilinear transforms of the BS.1770
    // analog prototypes (as in libebur128), so any sample rate matches the spec's 48 kHz coefficients.
    const double pi = 3.14159265358979323846;
    auto highShelf = [&]() {
        const double K = std::tan(pi * 1681.974450955533 / sampleRate), Q = 0.7071752369554196;
        const double Vh = std::pow(10.0, 3.999843853973347 / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        return Biquad{(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                      2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    };
    auto highPass = [&]() {
        const double K = std::tan(pi * 38.13547087602444 / sampleRate), Q = 0.5003270373238773;
        const double a0 = 1.0 + K / Q + K * K;
        return Biquad{1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    };

    const size_t frames = interleaved.size() / 2;
    const size_t window = sampleRate * 4 / 10, hop = sampleRate / 10;
    std::vector<double> blocks;
    if (frames < window) return blocks;

    std::vector<double> power(frames);
    Biquad shelf[2] = {highShelf(), highShelf()}, hp[2] = {highPass(), highPass()};
    for (size_t i = 0; i < frames; ++i) {
        double p = 0.0;
        for (int ch = 0; ch < 2; ++ch) {
            const double y = hp[ch].process(shelf[ch].process(interleaved[2 * i + ch]));
            p += y * y;
        }
        power[i] = p;
    }

    double windowSum = 0.0;
    for (size_t i = 0; i < window; ++i) windowSum += power[i];
    for (size_t start = 0;; start += hop) {
        blocks.push_back(windowSum / window);
        if (start + hop + window > frames) break;
        for (size_t i = 0; i < hop; ++i) windowSum += power[start + window + i] - power[start + i];
    }
    return blocks;
}

inline double powerToLufs(double meanPower) {
    return meanPower > 0.0 ? -0.691 + 10.0 * std::log10(meanPower) : -999.0;
}

// Loudest BS.1770 momentary loudness (400 ms window) of interleaved stereo, in LUFS. -999 for silence
// or input shorter than one window.
inline double maxMomentaryLoudness(const std::vector<float>& interleaved, uint32_t sampleRate) {
    const std::vector<double> blocks = loudnessBlockPowers(interleaved, sampleRate);
    return powerToLufs(blocks.empty() ? 0.0 : *std::max_element(blocks.begin(), blocks.end()));
}

// BS.1770 integrated loudness of interleaved stereo, in LUFS: the mean over the whole signal, gated to
// ignore silence (-70 LUFS) and quiet tails (10 LU below the ungated mean). Unlike the momentary peak it
// rates a fast-decaying sound quieter than a sustained one with the same attack, as the ear does.
inline double integratedLoudness(const std::vector<float>& interleaved, uint32_t sampleRate) {
    const std::vector<double> blocks = loudnessBlockPowers(interleaved, sampleRate);
    auto gatedMean = [&](double thresholdLufs) {
        double sum = 0.0;
        size_t n = 0;
        for (double p : blocks) {
            if (powerToLufs(p) > thresholdLufs) { sum += p; ++n; }
        }
        return n ? sum / n : 0.0;
    };
    const double absolute = gatedMean(-70.0);
    if (absolute <= 0.0) return -999.0;
    return powerToLufs(gatedMean(powerToLufs(absolute) - 10.0));
}

} // namespace SimUtil
