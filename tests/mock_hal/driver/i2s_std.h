#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#ifndef ESP_ERR_T_DEFINED
#define ESP_ERR_T_DEFINED
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
#endif

using gpio_num_t = int;
constexpr int I2S_GPIO_UNUSED = -1;

enum i2s_port_t {
    I2S_NUM_0 = 0,
    I2S_NUM_1 = 1,
    I2S_NUM_AUTO = 2
};

enum i2s_role_t {
    I2S_ROLE_MASTER = 0,
    I2S_ROLE_SLAVE = 1
};

enum i2s_data_bit_width_t {
    I2S_DATA_BIT_WIDTH_16BIT = 16,
    I2S_DATA_BIT_WIDTH_24BIT = 24,
    I2S_DATA_BIT_WIDTH_32BIT = 32
};

enum i2s_slot_mode_t {
    I2S_SLOT_MODE_MONO = 1,
    I2S_SLOT_MODE_STEREO = 2
};

struct i2s_chan_config_t {
    i2s_port_t id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear;
};

#define I2S_CHANNEL_DEFAULT_CONFIG(port, r) { \
    .id = port, \
    .role = r, \
    .dma_desc_num = 8, \
    .dma_frame_num = 128, \
    .auto_clear = true \
}

struct i2s_std_gpio_config_t {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    gpio_num_t din;
    struct {
        bool mclk_inv;
        bool bclk_inv;
        bool ws_inv;
    } invert_flags;
};

struct i2s_std_clk_config_t {
    uint32_t sample_rate_hz;
    uint32_t clk_src;
    uint32_t mclk_multiple;
};

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { \
    .sample_rate_hz = rate, \
    .clk_src = 0, \
    .mclk_multiple = 256 \
}

struct i2s_std_slot_config_t {
    i2s_data_bit_width_t data_bit_width;
    i2s_data_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
};

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) { \
    .data_bit_width = bits, \
    .slot_bit_width = bits, \
    .slot_mode = mode \
}

struct i2s_std_config_t {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
};

using i2s_chan_handle_t = void*;

// Mock instrumentation state
struct MockI2SStd {
    static inline bool installed = false;
    static inline bool enabled = false;
    static inline uint32_t configuredSampleRate = 0;
    static inline i2s_data_bit_width_t configuredBitWidth = I2S_DATA_BIT_WIDTH_16BIT;
    static inline i2s_std_gpio_config_t lastPins = {};
    static inline std::vector<int32_t> capturedAudio32; // Captured 32-bit slot stereo frames
    static inline std::vector<int16_t> capturedAudio16; // Captured 16-bit slot stereo frames

    static void reset() {
        installed = false;
        enabled = false;
        configuredSampleRate = 0;
        configuredBitWidth = I2S_DATA_BIT_WIDTH_16BIT;
        lastPins = {};
        capturedAudio32.clear();
        capturedAudio16.clear();
    }
};

inline esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg, i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle) {
    MockI2SStd::installed = true;
    if (tx_handle) *tx_handle = reinterpret_cast<void*>(0x5678);
    return ESP_OK;
}

inline esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg) {
    if (!std_cfg) return ESP_FAIL;
    MockI2SStd::configuredSampleRate = std_cfg->clk_cfg.sample_rate_hz;
    MockI2SStd::configuredBitWidth = std_cfg->slot_cfg.data_bit_width;
    MockI2SStd::lastPins = std_cfg->gpio_cfg;
    return ESP_OK;
}

inline esp_err_t i2s_channel_enable(i2s_chan_handle_t handle) {
    MockI2SStd::enabled = true;
    return ESP_OK;
}

inline esp_err_t i2s_channel_disable(i2s_chan_handle_t handle) {
    MockI2SStd::enabled = false;
    return ESP_OK;
}

inline esp_err_t i2s_del_channel(i2s_chan_handle_t handle) {
    MockI2SStd::installed = false;
    MockI2SStd::enabled = false;
    return ESP_OK;
}

inline esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size, size_t *bytes_written, uint32_t ticks_to_wait) {
    if (!src || size == 0) return ESP_FAIL;
    if (bytes_written) *bytes_written = size;

    if (MockI2SStd::configuredBitWidth == I2S_DATA_BIT_WIDTH_32BIT) {
        size_t samples = size / sizeof(int32_t);
        const int32_t* p = reinterpret_cast<const int32_t*>(src);
        MockI2SStd::capturedAudio32.insert(MockI2SStd::capturedAudio32.end(), p, p + samples);
    } else {
        size_t samples = size / sizeof(int16_t);
        const int16_t* p = reinterpret_cast<const int16_t*>(src);
        MockI2SStd::capturedAudio16.insert(MockI2SStd::capturedAudio16.end(), p, p + samples);
    }

    return ESP_OK;
}
