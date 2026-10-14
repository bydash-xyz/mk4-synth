#include "SineLut.h"

namespace Synth {

float SineLut::s_table[SINE_LUT_SIZE];
bool SineLut::s_initialized = false;

void SineLut::init() {
    if (s_initialized) return;
    for (int i = 0; i < SINE_LUT_SIZE; i++) {
        s_table[i] = std::sin(static_cast<float>(i) * TWO_PI_F / static_cast<float>(SINE_LUT_SIZE));
    }
    s_initialized = true;
}

} // namespace Synth
