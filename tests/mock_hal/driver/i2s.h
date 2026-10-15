#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <functional>

// ESP-IDF & FreeRTOS types
#ifndef ESP_ERR_T_DEFINED
#define ESP_ERR_T_DEFINED
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
#endif

#ifndef FREERTOS_MOCKS_DEFINED
#define FREERTOS_MOCKS_DEFINED

using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);

#ifndef configMAX_PRIORITIES
#define configMAX_PRIORITIES 25
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xFFFFFFFFUL
#endif

inline uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }

// FreeRTOS Task Mock
struct MockFreeRTOS {
    static inline TaskFunction_t lastTaskFunction = nullptr;
    static inline void* lastTaskParam = nullptr;
    static inline const char* lastTaskName = nullptr;
    static inline int lastPinnedCore = -1;

    static void reset() {
        lastTaskFunction = nullptr;
        lastTaskParam = nullptr;
        lastTaskName = nullptr;
        lastPinnedCore = -1;
    }
};

inline void xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char* name,
    uint32_t stack,
    void* param,
    uint32_t prio,
    TaskHandle_t* handle,
    int core
) {
    MockFreeRTOS::lastTaskFunction = task;
    MockFreeRTOS::lastTaskParam = param;
    MockFreeRTOS::lastTaskName = name;
    MockFreeRTOS::lastPinnedCore = core;
    if (handle) *handle = reinterpret_cast<TaskHandle_t>(0x1234);
}

inline void vTaskDelay(uint32_t ticks) {}

#endif // FREERTOS_MOCKS_DEFINED

// I2S enums and constants
enum i2s_port_t {
    I2S_NUM_0 = 0,
    I2S_NUM_1 = 1
};

enum i2s_mode_t {
    I2S_MODE_MASTER = 1 << 0,
    I2S_MODE_TX     = 1 << 2
};

enum i2s_bits_per_sample_t {
    I2S_BITS_PER_SAMPLE_16BIT = 16
};

enum i2s_channel_fmt_t {
    I2S_CHANNEL_FMT_RIGHT_LEFT = 0
};

enum i2s_comm_format_t {
    I2S_COMM_FORMAT_STAND_I2S = 0x01
};

enum i2s_channel_t {
    I2S_CHANNEL_STEREO = 2
};

constexpr int ESP_INTR_FLAG_LEVEL1 = 1;
constexpr int I2S_PIN_NO_CHANGE = -1;

struct i2s_config_t {
    i2s_mode_t mode;
    uint32_t sample_rate;
    i2s_bits_per_sample_t bits_per_sample;
    i2s_channel_fmt_t channel_format;
    i2s_comm_format_t communication_format;
    int intr_alloc_flags;
    int dma_buf_count;
    int dma_buf_len;
    bool use_apll;
    bool tx_desc_auto_clear;
};

struct i2s_pin_config_t {
    int bck_io_num;
    int ws_io_num;
    int data_out_num;
    int data_in_num;
};

// Mock I2S Peripheral Recorder for Unit Tests
struct MockI2S {
    static inline bool installed = false;
    static inline i2s_config_t lastConfig{};
    static inline i2s_pin_config_t lastPins{};
    static inline uint32_t configuredSampleRate = 0;
    static inline std::vector<int16_t> capturedAudio;
    static inline esp_err_t writeError = ESP_OK;
    static inline size_t writeBytesWrittenOverride = 0;
    static inline bool writeBytesOverrideActive = false;

    static void reset() {
        installed = false;
        configuredSampleRate = 0;
        capturedAudio.clear();
        writeError = ESP_OK;
        writeBytesWrittenOverride = 0;
        writeBytesOverrideActive = false;
    }
};

inline esp_err_t i2s_driver_install(i2s_port_t port, const i2s_config_t* config, int queue_size, void* queue) {
    MockI2S::installed = true;
    if (config) MockI2S::lastConfig = *config;
    return ESP_OK;
}

inline esp_err_t i2s_set_pin(i2s_port_t port, const i2s_pin_config_t* pin_config) {
    if (pin_config) MockI2S::lastPins = *pin_config;
    return ESP_OK;
}

inline esp_err_t i2s_set_clk(i2s_port_t port, uint32_t rate, i2s_bits_per_sample_t bits, i2s_channel_t ch) {
    MockI2S::configuredSampleRate = rate;
    return ESP_OK;
}

inline esp_err_t i2s_write(i2s_port_t port, const void* src, size_t size, size_t* bytes_written, uint32_t ticks_to_wait) {
    if (MockI2S::writeError != ESP_OK) {
        if (bytes_written) *bytes_written = 0;
        return MockI2S::writeError;
    }
    if (MockI2S::writeBytesOverrideActive) {
        if (bytes_written) *bytes_written = MockI2S::writeBytesWrittenOverride;
        return ESP_OK;
    }
    const int16_t* samples = static_cast<const int16_t*>(src);
    size_t count = size / sizeof(int16_t);
    MockI2S::capturedAudio.insert(MockI2S::capturedAudio.end(), samples, samples + count);
    if (bytes_written) *bytes_written = size;
    return ESP_OK;
}
