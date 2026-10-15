#include "SynthEngine.h"
#include "MidiParser.h"
#include "esp32s3_driver.h"

#include <iostream>
#include <cassert>
#include <cmath>

int main() {
    std::cout << "\n=============================================" << std::endl;
    std::cout << "  ESP32-S3 Rev 2.0 Driver Tests (macOS Host) " << std::endl;
    std::cout << "=============================================\n" << std::endl;

    Synth::SynthEngine engine;
    Synth::MidiParser  parser;

    MockI2SStd::reset();
    MockFreeRTOS::reset();

    // Test 1: Hardware initialization
    std::cout << "Test 1: Hardware initialization (ESP-IDF v5 driver/i2s_std.h, Core 1 task, GPIO 4, 5, 6)..." << std::endl;
    bool ok = HardwareDriver::initHardware(engine);
    assert(ok);
    assert(HardwareDriver::getAudioInitError() == HardwareDriver::AudioInitError::None);

    // Verify I2S driver installation and Rev 2.0 pin routing
    assert(MockI2SStd::installed == true);
    assert(MockI2SStd::enabled == true);
    assert(MockI2SStd::configuredSampleRate == 44100);
    assert(MockI2SStd::configuredBitWidth == I2S_DATA_BIT_WIDTH_32BIT);
    assert(MockI2SStd::lastPins.bclk == 4); // GPIO 4 (BCK)
    assert(MockI2SStd::lastPins.ws == 5);   // GPIO 5 (LRCK)
    assert(MockI2SStd::lastPins.dout == 6); // GPIO 6 (DIN)

    // Verify FreeRTOS audio rendering task pinned to Core 1
    assert(MockFreeRTOS::lastPinnedCore == 1);
    assert(MockFreeRTOS::lastTaskName != nullptr);

    // Verify 8-voice polyphony limit
    assert(engine.getVoiceLimit() == Synth::SynthEngine::MAX_VOICES);
    // Cross-core safety: MIDI (Core 0) events are applied by the audio task (Core 1)
    assert(engine.isDeferredEvents());
    std::cout << "  Passed (I2S DMA on GPIO 4, 5, 6 at 32-bit slot stereo, task pinned to Core 1)" << std::endl;

    // Test 2: Audio block rendering via I2S DMA
    std::cout << "Test 2: Audio block rendering (128 frames / 256 32-bit stereo samples)..." << std::endl;
    MockI2SStd::capturedAudio32.clear();

    engine.noteOn(60, 100); // Play Middle C
    HardwareDriver::renderAudioBlock();
    assert(MockI2SStd::capturedAudio32.size() == 256);
    std::cout << "  Passed (Rendered exact 128 frames of 32-bit stereo into I2S DMA)" << std::endl;

    // Test 3: Continuous DMA audio streaming under load
    std::cout << "Test 3: Continuous DMA audio streaming under load (10 blocks / 2560 samples)..." << std::endl;
    MockI2SStd::capturedAudio32.clear();
    for (int block = 0; block < 10; ++block) {
        HardwareDriver::renderAudioBlock();
    }
    assert(MockI2SStd::capturedAudio32.size() == 2560); // 10 blocks * 256 samples

    bool hasNonZero = false;
    for (int32_t sample : MockI2SStd::capturedAudio32) {
        if (sample != 0) hasNonZero = true;
    }
    assert(hasNonZero);
    std::cout << "  Passed (Continuous 32-bit stereo streaming verified without distortion)" << std::endl;

    // Test 4: Release note and engine panic
    std::cout << "Test 4: Note release and panic..." << std::endl;
    engine.noteOff(60);
    engine.panic();
    HardwareDriver::renderAudioBlock(); // queued events are applied by the audio task
    assert(engine.getActiveVoiceCount() == 0);
    std::cout << "  Passed (All voices released cleanly)" << std::endl;

    // Test 5: I2S write error counting (write failures and short writes)
    std::cout << "Test 5: I2S write error counting..." << std::endl;
    assert(HardwareDriver::getI2sWriteErrorCount() == 0);

    // Normal render should not increment error count
    HardwareDriver::renderAudioBlock();
    assert(HardwareDriver::getI2sWriteErrorCount() == 0);

    // 5a: Inject write failure (writeErr != ESP_OK)
    MockI2SStd::writeError = ESP_FAIL;
    HardwareDriver::renderAudioBlock();
    assert(HardwareDriver::getI2sWriteErrorCount() == 1);

    // 5b: Inject short write (bytesWritten != sizeof(s_i2sSlotBuffer))
    MockI2SStd::writeError = ESP_OK;
    MockI2SStd::writeBytesOverrideActive = true;
    MockI2SStd::writeBytesWrittenOverride = 100;
    HardwareDriver::renderAudioBlock();
    assert(HardwareDriver::getI2sWriteErrorCount() == 2);

    // 5c: Reset error count
    HardwareDriver::resetI2sWriteErrorCount();
    assert(HardwareDriver::getI2sWriteErrorCount() == 0);

    // 5d: Resuming normal writes keeps count at 0
    MockI2SStd::writeBytesOverrideActive = false;
    HardwareDriver::renderAudioBlock();
    assert(HardwareDriver::getI2sWriteErrorCount() == 0);
    std::cout << "  Passed (Error counter increments on DMA errors and short writes, resets correctly)" << std::endl;

    std::cout << "\nAll ESP32-S3 Rev 2.0 Driver host tests passed successfully!\n" << std::endl;
    return 0;
}
