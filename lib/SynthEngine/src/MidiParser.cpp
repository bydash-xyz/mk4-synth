#include "MidiParser.h"

namespace Synth {

MidiParser::MidiParser() {
    reset();
}

void MidiParser::reset() {
    runningStatus_ = 0;
    currentStatus_ = 0;
    dataByte1_ = 0;
    expectedDataBytes_ = 0;
    receivedDataBytes_ = 0;
    inSysEx_ = false;
    sysexBuffer_.clear();
}

void MidiParser::parseBuffer(const uint8_t* data, size_t length, SynthEngine& engine) {
    for (size_t i = 0; i < length; ++i) {
        parseByte(data[i], engine);
    }
}

void MidiParser::parseByte(uint8_t byte, SynthEngine& engine) {
    // 1. Real-time messages (0xF8 - 0xFF) can arrive anywhere and do not reset running status or interrupt SysEx
    if (byte >= 0xF8) {
        switch (byte) {
            case 0xF8: // Timing Clock (24 PPQN)
                engine.midiClockTick();
                break;
            case 0xFA: // Start
            case 0xFB: // Continue
                engine.midiClockStart();
                break;
            case 0xFC: // Stop
                engine.midiClockStop();
                break;
            case 0xFF: // System Reset
                engine.panic();
                reset();
                break;
            default:
                break;
        }
        return;
    }

    // If currently accumulating a SysEx message
    if (inSysEx_) {
        if (byte == 0xF7) { // End of SysEx
            sysexBuffer_.push_back(0xF7);
            engine.handleSysEx(sysexBuffer_.data(), sysexBuffer_.size());
            inSysEx_ = false;
            sysexBuffer_.clear();
            return;
        } else if (byte & 0x80) {
            // Any other status byte (0x80 - 0xF6) aborts SysEx reception per MIDI spec
            inSysEx_ = false;
            sysexBuffer_.clear();
            // Fall through to process as regular status byte below
        } else {
            // Normal 7-bit data byte
            if (sysexBuffer_.size() < MAX_SYSEX_SIZE) {
                sysexBuffer_.push_back(byte);
            }
            return;
        }
    }

    // 2. Status byte received (MSB = 1)
    if (byte & 0x80) {
        if (byte == 0xF0) { // Start of SysEx
            inSysEx_ = true;
            runningStatus_ = 0;
            currentStatus_ = 0;
            receivedDataBytes_ = 0;
            sysexBuffer_.clear();
            sysexBuffer_.push_back(0xF0);
            return;
        }

        // Other System Common messages (0xF1 - 0xF7) cancel running status
        if (byte >= 0xF1 && byte <= 0xF7) {
            runningStatus_ = 0;
            currentStatus_ = 0;
            receivedDataBytes_ = 0;
            return;
        }

        currentStatus_ = byte;
        runningStatus_ = byte;
        receivedDataBytes_ = 0;

        uint8_t msgType = currentStatus_ & 0xF0;
        if (msgType == 0xC0 || msgType == 0xD0) {
            expectedDataBytes_ = 1; // Program Change, Channel Pressure
        } else {
            expectedDataBytes_ = 2; // Note Off, Note On, Aftertouch, CC, Pitch Bend
        }
        return;
    }

    // 3. Data byte received (MSB = 0)
    // If no active status, check running status
    if (currentStatus_ == 0) {
        if (runningStatus_ != 0) {
            currentStatus_ = runningStatus_;
            receivedDataBytes_ = 0;
            uint8_t msgType = currentStatus_ & 0xF0;
            if (msgType == 0xC0 || msgType == 0xD0) {
                expectedDataBytes_ = 1;
            } else {
                expectedDataBytes_ = 2;
            }
        } else {
            // Stray data byte with no status - ignore
            return;
        }
    }

    if (receivedDataBytes_ == 0) {
        dataByte1_ = byte;
        receivedDataBytes_ = 1;

        if (expectedDataBytes_ == 1) {
            dispatchMessage(engine);
            receivedDataBytes_ = 0; // ready for running status next data byte
        }
    } else if (receivedDataBytes_ == 1 && expectedDataBytes_ == 2) {
        uint8_t dataByte2 = byte;
        receivedDataBytes_ = 0; // ready for next packet under running status

        uint8_t msgType = currentStatus_ & 0xF0;
        switch (msgType) {
            case 0x80: // Note Off
                engine.noteOff(dataByte1_);
                break;

            case 0x90: // Note On (velocity 0 is Note Off)
                if (dataByte2 == 0) {
                    engine.noteOff(dataByte1_);
                } else {
                    engine.noteOn(dataByte1_, dataByte2);
                }
                break;

            case 0xB0: // Control Change
                engine.controlChange(dataByte1_, dataByte2);
                break;

            case 0xE0: { // Pitch Bend (14-bit: LSB + MSB << 7)
                int16_t bend = static_cast<int16_t>(dataByte1_ | (dataByte2 << 7));
                engine.pitchBend(bend);
                break;
            }

            default:
                break;
        }
    }
}

void MidiParser::dispatchMessage(SynthEngine& engine) {
    uint8_t msgType = currentStatus_ & 0xF0;
    if (msgType == 0xC0) {
        // Program Change (0xC0 [prog]): switch synth model / preset
        engine.loadPresetIndex(dataByte1_);
    }
}

} // namespace Synth
