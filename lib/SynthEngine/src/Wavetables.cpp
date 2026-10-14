#include "Wavetables.h"
#include "SineLut.h"
#include <algorithm>
#include <cmath>

namespace Synth {

std::vector<int16_t> Wavetables::s_data;

namespace {

constexpr int kTopHarmonic = Wavetables::MAX_HARMONICS[0];

// Amplitude of harmonic k (1-based) in frame `f` of `table`. Negative values invert the partial.
float harmonicAmplitude(int table, int f, int k) {
    const float u  = static_cast<float>(f) / static_cast<float>(Wavetables::FRAMES - 1); // 0..1 across the table
    const float kf = static_cast<float>(k);
    switch (table) {
        case 0: { // "PPG Sweep": saw body with a resonant peak climbing from the 2nd to the 32nd harmonic
            const float centre = 2.0f + u * 30.0f;
            const float width  = 1.2f + u * 2.5f;
            const float d      = (kf - centre) / width;
            return 0.45f / kf + 1.1f * std::exp(-0.5f * d * d) / (1.0f + 0.03f * centre);
        }
        case 1: { // "Analog": saw, saw+square, square, then pulses narrowing to 6 %
            static const float widths[Wavetables::FRAMES] = {0.0f, 0.0f, 0.5f, 0.4f, 0.3f, 0.2f, 0.12f, 0.06f};
            const float pi = 3.14159265f;
            const float saw = 1.0f / kf;
            const float square = std::sin(pi * kf * 0.5f) / kf;
            if (f == 0) return saw;
            if (f == 1) return 0.5f * saw + 0.5f * square;
            return std::sin(pi * kf * widths[f]) / kf;
        }
        case 2: { // "Vox": three formants morphing U -> O -> A -> E -> I (positions for a ~150 Hz voice)
            static const float formants[5][3] = {
                {2.2f, 4.7f, 16.9f},  // U
                {3.0f, 5.3f, 18.9f},  // O
                {5.3f, 7.7f, 19.3f},  // A
                {2.7f, 10.7f, 18.0f}, // E
                {1.8f, 15.3f, 20.0f}, // I
            };
            const float pos = u * 4.0f;
            const int   v0  = std::min(3, static_cast<int>(pos));
            const float t   = pos - static_cast<float>(v0);
            static const float gains[3] = {1.0f, 0.55f, 0.22f};
            float a = 0.03f / kf;
            for (int i = 0; i < 3; ++i) {
                const float fc = formants[v0][i] + t * (formants[v0 + 1][i] - formants[v0][i]);
                const float bw = 0.9f + 0.07f * fc;
                const float d  = (kf - fc) / bw;
                a += gains[i] * std::exp(-0.5f * d * d);
            }
            return a / std::pow(kf, 0.3f);
        }
        default: { // "Glass": sparse, widening harmonic clusters (digital bell / PPG upper waves)
            float a = (k == 1) ? 1.0f - 0.6f * u : 0.0f;
            for (int j = 1; j <= 4; ++j) {
                const int h = 1 + static_cast<int>(std::lround(static_cast<float>(j) * (1.6f + 2.4f * u) + 3.0f * u * u * static_cast<float>(j)));
                if (h == k) a += 0.9f / (1.0f + 0.35f * static_cast<float>(j - 1));
            }
            return a;
        }
    }
}

} // namespace

void Wavetables::init() {
    if (!s_data.empty()) return;
    SineLut::init();
    s_data.assign(static_cast<size_t>(WAVETABLE_COUNT) * LEVELS * FRAMES * STRIDE, 0);

    std::vector<float> acc(SIZE);
    std::vector<float> levels(static_cast<size_t>(LEVELS) * SIZE);
    for (int t = 0; t < static_cast<int>(WAVETABLE_COUNT); ++t) {
        for (int f = 0; f < FRAMES; ++f) {
            // Accumulate harmonics in order; whenever k reaches a level's limit, that prefix is the level
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (int k = 1; k <= kTopHarmonic; ++k) {
                const float a = harmonicAmplitude(t, f, k);
                if (a != 0.0f) {
                    for (int n = 0; n < SIZE; ++n) {
                        // k * n / SIZE cycles: exact multiples of the sine table's step (2048 / 128 = 16)
                        const int cycles = (k * n) % SIZE;
                        acc[n] += a * SineLut::lookupFast(TWO_PI_F * static_cast<float>(cycles) / static_cast<float>(SIZE));
                    }
                }
                for (int l = 0; l < LEVELS; ++l) {
                    if (MAX_HARMONICS[l] == k) std::copy(acc.begin(), acc.end(), levels.begin() + l * SIZE);
                }
            }
            // One gain per frame across its levels, so changing level (note range) does not change loudness
            float peak = 1.0e-6f;
            for (float v : levels) peak = std::max(peak, std::fabs(v));
            const float gain = 32000.0f / peak;
            for (int l = 0; l < LEVELS; ++l) {
                int16_t* out = &s_data[((static_cast<size_t>(t) * LEVELS + l) * FRAMES + f) * STRIDE];
                for (int n = 0; n < SIZE; ++n) {
                    out[n] = static_cast<int16_t>(std::lround(levels[l * SIZE + n] * gain));
                }
                out[SIZE] = out[0]; // guard sample for interpolation
            }
        }
    }
}

const char* Wavetables::tableName(uint8_t table) {
    static const char* const kNames[WAVETABLE_COUNT] = {"PPG SWEEP", "ANALOG", "VOX", "GLASS"};
    return table < WAVETABLE_COUNT ? kNames[table] : "?";
}

} // namespace Synth
