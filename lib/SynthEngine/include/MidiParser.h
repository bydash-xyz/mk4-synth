#pragma once

#include "SynthEngine.h"
#include <cstdint>
#include <vector>

namespace Synth {

class MidiParser {
public:
    static constexpr size_t MAX_SYSEX_SIZE = 512; // PatchDumpV2 maximum (MAX_SYSEX_MESSAGE_SIZE)

    MidiParser();

    // Reset parser state machine
    void reset();

    // Process a single byte from a serial stream or MIDI packet
    void parseByte(uint8_t byte, SynthEngine& engine);

    // Convenience method to parse an array or vector of bytes
    void parseBuffer(const uint8_t* data, size_t length, SynthEngine& engine);

private:
    uint8_t runningStatus_{0};
    uint8_t currentStatus_{0};
    uint8_t dataByte1_{0};
    uint8_t expectedDataBytes_{0};
    uint8_t receivedDataBytes_{0};

    bool inSysEx_{false};
    std::vector<uint8_t> sysexBuffer_;

    void dispatchMessage(SynthEngine& engine);
};

} // namespace Synth
