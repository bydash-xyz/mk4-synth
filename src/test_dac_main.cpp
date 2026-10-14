#include <Arduino.h>
#include <driver/i2s_std.h>
#include <cmath>

// =========================================================================
// Hardware Diagnostic 02: PCM5102A I2S DAC Audio Tone & Channel Test
// Hardware Connections:
//   PCM5102A VIN  <---> ESP32-S3 5V (or 3.3V)
//   PCM5102A GND  <---> ESP32-S3 GND
//   PCM5102A SCK  <---> ESP32-S3 GND  (MANDATORY: Activates internal PLL)
//   PCM5102A BCK  <---> ESP32-S3 GPIO 4
//   PCM5102A LRCK <---> ESP32-S3 GPIO 5
//   PCM5102A DIN  <---> ESP32-S3 GPIO 6
//   PCM5102A XSMT <---> ESP32-S3 3.3V (Unmute)
//   PCM5102A FMT  <---> ESP32-S3 GND  (Standard I2S)
// =========================================================================

constexpr int I2S_BCK_IO  = 4;
constexpr int I2S_WS_IO   = 5;
constexpr int I2S_DO_IO   = 6;

constexpr uint32_t SAMPLE_RATE = 44100;
constexpr size_t   BLOCK_FRAMES = 128;

static i2s_chan_handle_t s_txHandle = nullptr;
static int32_t s_audioBuffer[BLOCK_FRAMES * 2]; // Interleaved stereo 32-bit

enum class TestMode {
    STEREO_440,
    LEFT_ONLY_440,
    RIGHT_ONLY_440,
    DUAL_TONE_440_880
};

static TestMode s_mode = TestMode::STEREO_440;
static uint32_t s_modeChangeMillis = 0;
static float s_phaseL = 0.0f;
static float s_phaseR = 0.0f;

bool setupI2S() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = BLOCK_FRAMES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_txHandle, nullptr);
    if (err != ESP_OK) return false;

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
    if (err != ESP_OK) return false;

    err = i2s_channel_enable(s_txHandle);
    return (err == ESP_OK);
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) delay(10);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S3 Rev 2.0 Hardware Diagnostic #02: I2S DAC Test");
    Serial.println("=======================================================\n");
    Serial.printf("Configuring I2S: BCK=GPIO%d, LRCK=GPIO%d, DIN=GPIO%d @ %u Hz...\n",
                  I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, SAMPLE_RATE);

    if (setupI2S()) {
        Serial.println("-> [PASS] I2S Transmitter initialized and enabled successfully!");
        Serial.println("\nPlug standard stereo headphones into the PCM5102A 3.5mm jack.");
        Serial.println("Listening cycle every 3 seconds:");
        Serial.println("  1. Stereo 440 Hz (Both ears)");
        Serial.println("  2. Left Channel 440 Hz Only");
        Serial.println("  3. Right Channel 440 Hz Only");
        Serial.println("  4. Dual Tone: Left=440 Hz (A4), Right=880 Hz (A5)");
    } else {
        Serial.println("-> [FAIL] Failed to initialize I2S channel!");
    }
}

void loop() {
    // Mode switcher every 3 seconds
    if (millis() - s_modeChangeMillis > 3000) {
        s_modeChangeMillis = millis();
        switch (s_mode) {
            case TestMode::STEREO_440:
                s_mode = TestMode::LEFT_ONLY_440;
                Serial.println("[AUDIO] >> Mode: LEFT EAR ONLY (440 Hz)");
                break;
            case TestMode::LEFT_ONLY_440:
                s_mode = TestMode::RIGHT_ONLY_440;
                Serial.println("[AUDIO] >> Mode: RIGHT EAR ONLY (440 Hz)");
                break;
            case TestMode::RIGHT_ONLY_440:
                s_mode = TestMode::DUAL_TONE_440_880;
                Serial.println("[AUDIO] >> Mode: DUAL TONE (L=440 Hz A4, R=880 Hz A5)");
                break;
            case TestMode::DUAL_TONE_440_880:
                s_mode = TestMode::STEREO_440;
                Serial.println("[AUDIO] >> Mode: STEREO CENTER (440 Hz Both Ears)");
                break;
        }
    }

    float freqL = 440.0f;
    float freqR = (s_mode == TestMode::DUAL_TONE_440_880) ? 880.0f : 440.0f;
    float stepL = freqL * 2.0f * M_PI / SAMPLE_RATE;
    float stepR = freqR * 2.0f * M_PI / SAMPLE_RATE;

    // Amplitude: -12 dBFS (~0.25) to protect ears
    constexpr float AMP = 0.25f;

    for (size_t i = 0; i < BLOCK_FRAMES; ++i) {
        float sampleL = 0.0f;
        float sampleR = 0.0f;

        if (s_mode == TestMode::STEREO_440 || s_mode == TestMode::LEFT_ONLY_440 || s_mode == TestMode::DUAL_TONE_440_880) {
            sampleL = std::sin(s_phaseL) * AMP;
        }
        if (s_mode == TestMode::STEREO_440 || s_mode == TestMode::RIGHT_ONLY_440 || s_mode == TestMode::DUAL_TONE_440_880) {
            sampleR = std::sin(s_phaseR) * AMP;
        }

        s_phaseL += stepL;
        if (s_phaseL >= 2.0f * M_PI) s_phaseL -= 2.0f * M_PI;
        s_phaseR += stepR;
        if (s_phaseR >= 2.0f * M_PI) s_phaseR -= 2.0f * M_PI;

        int16_t pcm16L = static_cast<int16_t>(sampleL * 32767.0f);
        int16_t pcm16R = static_cast<int16_t>(sampleR * 32767.0f);

        // Put 16-bit PCM into upper 16 bits of 32-bit I2S slot
        s_audioBuffer[i * 2 + 0] = static_cast<int32_t>(pcm16L) << 16;
        s_audioBuffer[i * 2 + 1] = static_cast<int32_t>(pcm16R) << 16;
    }

    size_t written = 0;
    i2s_channel_write(s_txHandle, s_audioBuffer, sizeof(s_audioBuffer), &written, portMAX_DELAY);
}
