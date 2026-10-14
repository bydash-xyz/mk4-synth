#pragma once

#include <cstdint>

namespace HardwareDriver {

// Which set of parameters the MK4's eight knobs control (shown on the synth's OLED)
enum class KnobPage : uint8_t {
    BANK_A = 0, // K1..K8 of the preset
    BANK_B = 1, // K9..K16 of the preset (BANK A/B button)
    RHYTHM = 2, // Metronome, tempo, time signature, key and scale (SHIFT + BANK A/B)
};

} // namespace HardwareDriver
