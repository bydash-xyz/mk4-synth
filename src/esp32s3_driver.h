#pragma once

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

#include <Arduino.h>
#include <atomic>

#if defined(MOCK_HAL) || __has_include(<driver/i2s_std.h>)
#include <driver/i2s_std.h>
#define SYNTH_USE_I2S_STD 1
#else
#include <driver/i2s.h>
#define SYNTH_USE_I2S_STD 0
#endif

#include "SynthEngine.h"
#include "MidiParser.h"
#include "profiling.h"

namespace HardwareDriver {

// -------------------------------------------------------------
// ESP32-S3 Rev 2.0 Pin & Audio Configuration (from HARDWARE.md)
// -------------------------------------------------------------
constexpr int I2S_BCK_IO        = 4; // Bit Clock (BCK) on GPIO 4
constexpr int I2S_WS_IO         = 5; // Word Select / Frame Clock (LRCK) on GPIO 5
constexpr int I2S_DO_IO         = 6; // Serial PCM Data Out (DIN) on GPIO 6

constexpr uint32_t SAMPLE_RATE      = 44100;
constexpr size_t   DMA_BLOCK_FRAMES = 128; // 128 stereo frames per block (standard low-latency audio)

// Internal state (inline for C++17 single-instance linkage across translation units)
inline Synth::SynthEngine* s_synthPtr = nullptr;
inline int16_t s_rawBuffer[DMA_BLOCK_FRAMES * 2];       // Interleaved stereo 16-bit [L, R, L, R, ...]
inline int32_t s_i2sSlotBuffer[DMA_BLOCK_FRAMES * 2];   // 32-bit slot stereo for PCM5102A DAC
inline TaskHandle_t s_audioTaskHandle = nullptr;
inline std::atomic<uint32_t> s_i2sWriteErrors{0};

// Why initHardware() failed (main.cpp maps it to a SystemFault for the OLED fault page)
enum class AudioInitError : uint8_t { None, I2s, AudioTask };
inline AudioInitError s_audioInitError = AudioInitError::None;
inline AudioInitError getAudioInitError() { return s_audioInitError; }
#if SYNTH_USE_I2S_STD
inline i2s_chan_handle_t s_txHandle = nullptr;
#else
inline bool s_i2sInstalled = false;
#endif

#if defined(ARDUINO_ARCH_ESP32)
// -------------------------------------------------------------
// Second render core: with 3+ sounding voices the engine hands half of them to this task on Core 0.
// Core 0 otherwise runs the USB host / BLE stacks and the low-priority OLED render task, while Core 1
// hosts the audio task plus Arduino's loop() (MIDI/BLE polling, LEDs). Keeps chords well inside the 2.9 ms block budget and leaves Core 1 time for loop().
// -------------------------------------------------------------
class Core0VoiceWorker : public Synth::VoiceWorker {
public:
    bool begin(Synth::SynthEngine& engine) {
        engine_ = &engine;
        return xTaskCreatePinnedToCore(taskEntry, "SynthVoiceWorker", 4096, this,
                                       configMAX_PRIORITIES - 2, &task_, 0) == pdPASS;
    }

    void start() override {
        ulTaskNotifyTake(pdTRUE, 0);              // drop a stale completion from a timed-out block
        audioTask_ = xTaskGetCurrentTaskHandle();
        xTaskNotifyGive(task_);
    }

    bool wait() override {
        return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) > 0;
    }

private:
    static void taskEntry(void* arg) {
        auto* self = static_cast<Core0VoiceWorker*>(arg);
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            self->engine_->renderWorkerShare();
            xTaskNotifyGive(self->audioTask_);
        }
    }

    Synth::SynthEngine* engine_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t audioTask_ = nullptr;
};
inline Core0VoiceWorker s_voiceWorker;
#endif

// -------------------------------------------------------------
// Audio Rendering Step (Callable directly by host unit tests)
// -------------------------------------------------------------
inline void renderAudioBlock() {
#if SYNTH_USE_I2S_STD
    if (s_synthPtr && s_txHandle) {
        // 1. Render 128 frames of 16-bit interleaved stereo audio from SynthEngine
        PROFILE_AUDIO_BEGIN();
        s_synthPtr->processBlock(s_rawBuffer, DMA_BLOCK_FRAMES);
        PROFILE_AUDIO_END(s_synthPtr);

        // 2. Expand 16-bit audio into 32-bit slots for optimal PCM5102A DAC dynamic range
        for (size_t i = 0; i < DMA_BLOCK_FRAMES * 2; ++i) {
            s_i2sSlotBuffer[i] = static_cast<int32_t>(s_rawBuffer[i]) << 16;
        }

        // 3. Transmit 32-bit stereo audio block to I2S DMA FIFO
        size_t bytesWritten = 0;
        esp_err_t writeErr = i2s_channel_write(
            s_txHandle,
            s_i2sSlotBuffer,
            sizeof(s_i2sSlotBuffer),
            &bytesWritten,
            portMAX_DELAY
        );
        if (writeErr != ESP_OK || bytesWritten != sizeof(s_i2sSlotBuffer)) {
            s_i2sWriteErrors.fetch_add(1, std::memory_order_relaxed);
            PROFILE_I2S_ERROR();
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    if (s_synthPtr && s_i2sInstalled) {
        PROFILE_AUDIO_BEGIN();
        s_synthPtr->processBlock(s_rawBuffer, DMA_BLOCK_FRAMES);
        PROFILE_AUDIO_END(s_synthPtr);
        for (size_t i = 0; i < DMA_BLOCK_FRAMES * 2; ++i) {
            s_i2sSlotBuffer[i] = static_cast<int32_t>(s_rawBuffer[i]) << 16;
        }
        size_t bytesWritten = 0;
        esp_err_t writeErr = i2s_write(
            I2S_NUM_0,
            s_i2sSlotBuffer,
            sizeof(s_i2sSlotBuffer),
            &bytesWritten,
            portMAX_DELAY
        );
        if (writeErr != ESP_OK || bytesWritten != sizeof(s_i2sSlotBuffer)) {
            s_i2sWriteErrors.fetch_add(1, std::memory_order_relaxed);
            PROFILE_I2S_ERROR();
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif
}

// -------------------------------------------------------------
// FreeRTOS Audio Rendering Task (Pinned to Core 1)
// -------------------------------------------------------------
static void audioTask(void* pvParameters) {
    (void)pvParameters;
    PROFILE_BOOT_BENCH();
    while (true) {
        renderAudioBlock();
    }
}

// -------------------------------------------------------------
// I2S Master Audio Driver Configuration
// -------------------------------------------------------------
static bool setupI2S() {
#if SYNTH_USE_I2S_STD
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 12;
    chan_cfg.dma_frame_num = DMA_BLOCK_FRAMES;
    chan_cfg.auto_clear    = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_txHandle, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[Error] Failed to allocate I2S channel: %d\n", err);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(I2S_BCK_IO),
            .ws   = static_cast<gpio_num_t>(I2S_WS_IO),
            .dout = static_cast<gpio_num_t>(I2S_DO_IO),
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };

    err = i2s_channel_init_std_mode(s_txHandle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[Error] Failed to initialize I2S standard mode: %d\n", err);
        i2s_del_channel(s_txHandle);
        s_txHandle = nullptr;
        return false;
    }

    err = i2s_channel_enable(s_txHandle);
    if (err != ESP_OK) {
        Serial.printf("[Error] Failed to enable I2S channel: %d\n", err);
        i2s_del_channel(s_txHandle);
        s_txHandle = nullptr;
        return false;
    }
#else
    i2s_config_t i2s_config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = DMA_BLOCK_FRAMES,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_DO_IO,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[Error] Failed to install I2S driver: %d\n", err);
        return false;
    }
    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[Error] Failed to set I2S pins: %d\n", err);
        return false;
    }
    s_i2sInstalled = true;
#endif

    Serial.printf("[I2S] PCM5102A active at %d Hz 32-bit slot stereo on GPIOs BCK=%d, LRCK=%d, DIN=%d\n",
                  SAMPLE_RATE, I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO);
    return true;
}

// -------------------------------------------------------------
// Public Driver API
// -------------------------------------------------------------
inline uint32_t getI2sWriteErrorCount() {
    return s_i2sWriteErrors.load(std::memory_order_relaxed);
}

inline void resetI2sWriteErrorCount() {
    s_i2sWriteErrors.store(0, std::memory_order_relaxed);
}

inline bool initHardware(Synth::SynthEngine& engine) {
    s_synthPtr = &engine;
    engine.init(SAMPLE_RATE);
    engine.setVoiceLimit(Synth::SynthEngine::MAX_VOICES); // 8 voices polyphony
    // MIDI is handled on other tasks than audio: queue voice changes so only the audio task (and its
    // voice worker, between start() and wait()) touches voices
    engine.setDeferredEvents(true);

    s_audioInitError = AudioInitError::None;
    resetI2sWriteErrorCount();

    // Initialize modern I2S master transmitter
    if (!setupI2S()) {
        s_audioInitError = AudioInitError::I2s;
        return false;
    }

#if defined(ARDUINO_ARCH_ESP32)
    if (s_voiceWorker.begin(engine)) {
        engine.setVoiceWorker(&s_voiceWorker);
        Serial.println("[Task] Voice worker pinned to Core 0 (shares chord rendering with the audio task)");
    } else {
        Serial.println("[Warning] Voice worker failed to start; rendering all voices on Core 1");
    }
#endif

    // Create real-time audio generation task pinned to Core 1
    BaseType_t res = xTaskCreatePinnedToCore(
        audioTask,
        "SynthAudioTask",
        8192,  // 8 KB stack for 8-voice polyphonic DSP (SVF filter + dual ADSRs + chorus)
        nullptr,
        configMAX_PRIORITIES - 1,
        &s_audioTaskHandle,
        1 // Core 1 (Dedicated Real-Time DSP)
    );

    if (res != pdPASS) {
        Serial.println("[Error] Failed to spawn audio task on Core 1");
        s_audioInitError = AudioInitError::AudioTask;
        return false;
    }

    Serial.println("[Task] Real-time audio rendering task pinned to Core 1");
    Serial.println(">>> ESP32-S3 Rev 2.0 Polyphonic Synth Ready! <<<");
    return true;
}

} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
