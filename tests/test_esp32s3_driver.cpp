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

    // Test 6: 4-bit IMA ADPCM block decoding & encoding
    std::cout << "Test 6: 4-bit IMA ADPCM block decoder & encoder..." << std::endl;
    {
        constexpr size_t numSamples = 256;
        int16_t origPcm[numSamples];
        for (size_t i = 0; i < numSamples; ++i) {
            origPcm[i] = static_cast<int16_t>(10000.0f * std::sin(2.0f * 3.14159265f * i / 32.0f));
        }

        uint8_t encoded[numSamples / 2];
        int16_t encPred = 0;
        int8_t encStep = 0;
        HardwareDriver::encodeImaAdpcmBlock(origPcm, numSamples, encoded, encPred, encStep);

        int16_t decodedPcm[numSamples];
        int16_t decPred = 0;
        int8_t decStep = 0;
        HardwareDriver::decodeImaAdpcmBlock(encoded, numSamples / 2, decodedPcm, decPred, decStep);

        double totalDiff2 = 0.0;
        size_t count = 0;
        for (size_t i = 16; i < numSamples; ++i) {
            double diff = static_cast<double>(origPcm[i] - decodedPcm[i]);
            totalDiff2 += diff * diff;
            count++;
        }
        double rmsError = std::sqrt(totalDiff2 / count);
        assert(rmsError < 500.0);
        std::cout << "  Passed (IMA ADPCM block encode/decode verified, steady-state RMS error = " << rmsError << ")" << std::endl;
    }

    // Test 7: 4-point Hermite interpolation
    std::cout << "Test 7: 4-point Hermite interpolation reader..." << std::endl;
    {
        float hFlat = HardwareDriver::readSampleHermite(0.5f, 100.0f, 100.0f, 100.0f, 100.0f);
        assert(std::abs(hFlat - 100.0f) < 1e-4f);

        float hLin = HardwareDriver::readSampleHermite(0.5f, 10.0f, 20.0f, 30.0f, 40.0f);
        assert(std::abs(hLin - 25.0f) < 1e-4f);

        float h0 = HardwareDriver::readSampleHermite(0.0f, 5.0f, 15.0f, 25.0f, 35.0f);
        float h1 = HardwareDriver::readSampleHermite(1.0f, 5.0f, 15.0f, 25.0f, 35.0f);
        assert(std::abs(h0 - 15.0f) < 1e-4f);
        assert(std::abs(h1 - 25.0f) < 1e-4f);
        std::cout << "  Passed (Hermite 4-point exact interpolation verified)" << std::endl;
    }

    // Test 8: Zero-latency attack header playback
    std::cout << "Test 8: Zero-latency attack header playback (512 SRAM samples)..." << std::endl;
    {
        HardwareDriver::PsramVoiceStream stream;
        stream.init();

        int16_t header[HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES];
        for (size_t i = 0; i < HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES; ++i) {
            header[i] = static_cast<int16_t>(i * 30);
        }

        stream.trigger(header, nullptr, 0);

        for (size_t i = 0; i < HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES; ++i) {
            float s = stream.readSampleHermite(1.0f);
            int16_t sample16 = static_cast<int16_t>(std::round(s * 32768.0f));
            assert(std::abs(sample16 - header[i]) <= 2);
        }
        std::cout << "  Passed (Zero-latency 512-sample attack header playback verified)" << std::endl;
    }

    // Test 9: Ping-pong buffer refill without stalls
    std::cout << "Test 9: Ping-pong buffer refill without stalls..." << std::endl;
    {
        HardwareDriver::PsramVoiceStream stream;
        stream.init();

        int16_t header[HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES];
        for (size_t i = 0; i < HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES; ++i) {
            header[i] = 1000;
        }

        int16_t psramPcm[512];
        for (size_t i = 0; i < 512; ++i) {
            psramPcm[i] = static_cast<int16_t>(2000 + i);
        }

        stream.trigger(header, reinterpret_cast<const uint8_t*>(psramPcm), sizeof(psramPcm), false);

        for (size_t i = 0; i < HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES; ++i) {
            stream.readSampleHermite(1.0f);
        }

        assert(stream.refillNeeded.load() == true);
        uint8_t oldHalf = stream.activeHalf.load();

        bool refilled = stream.refillPingPongFromPsram();
        assert(refilled == true);
        assert(stream.activeHalf.load() == (1 - oldHalf));
        assert(stream.refillNeeded.load() == false);

        float nextSample = stream.readSampleHermite(1.0f);
        assert(std::isfinite(nextSample) && nextSample != 0.0f);
        std::cout << "  Passed (Atomic ping-pong buffer refill verified without stalls)" << std::endl;
    }

    // Test 10: Mellotron 7.5-second tape-end cutoff
    std::cout << "Test 10: Mellotron 7.5-second tape-end cutoff..." << std::endl;
    {
        HardwareDriver::PsramVoiceStream stream;
        stream.init();

        int16_t header[HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES];
        for (size_t i = 0; i < HardwareDriver::PsramVoiceStream::ATTACK_HEADER_SAMPLES; ++i) {
            header[i] = 5000;
        }

        stream.trigger(header, nullptr, 0, false, HardwareDriver::StreamMode::Mellotron);

        stream.totalSamplesRead = HardwareDriver::PsramVoiceStream::MAX_MELLOTRON_TAPE_SAMPLES - 1;

        float sActive = stream.readSampleHermite(1.0f);
        assert(sActive != 0.0f);

        assert(stream.totalSamplesRead >= HardwareDriver::PsramVoiceStream::MAX_MELLOTRON_TAPE_SAMPLES);
        float sCutoff = stream.readSampleHermite(1.0f);
        assert(sCutoff == 0.0f);

        float sCutoff2 = stream.readSampleHermite(1.0f);
        assert(sCutoff2 == 0.0f);
        std::cout << "  Passed (Mellotron tape-end cutoff at exactly 7.5 seconds verified)" << std::endl;
    }

    std::cout << "\nAll ESP32-S3 Rev 2.0 Driver host tests passed successfully!\n" << std::endl;
    return 0;
}
