#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <cstdio>

// =========================================================================
// Hardware Diagnostic 03: SSD1306 0.96" OLED I2C Display & Bus Scan
// Hardware Connections:
//   SSD1306 VCC <---> ESP32-S3 3.3V
//   SSD1306 GND <---> ESP32-S3 GND
//   SSD1306 SDA <---> ESP32-S3 GPIO 8
//   SSD1306 SCL <---> ESP32-S3 GPIO 9
// =========================================================================

constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

static int ballX = 64;
static int ballY = 32;
static int ballDX = 2;
static int ballDY = 1;
static uint32_t frameCount = 0;
static uint32_t lastFpsMillis = 0;
static uint32_t currentFps = 0;

void scanI2CBus() {
    Serial.println("Scanning I2C Bus on GPIO 8 (SDA) and GPIO 9 (SCL)...");
    byte error, address;
    int nDevices = 0;

    for (address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();

        if (error == 0) {
            Serial.printf("  -> Found device at address 0x%02X", address);
            if (address == 0x3C || address == 0x3D) {
                Serial.print(" (SSD1306 OLED Display)");
            }
            Serial.println();
            nDevices++;
        }
    }

    if (nDevices == 0) {
        Serial.println("  -> [FAIL] No I2C devices found! Check wiring, pull-ups, or power.");
    } else {
        Serial.printf("  -> [PASS] Found %d device(s) on I2C bus.\n", nDevices);
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) delay(10);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S3 Rev 2.0 Hardware Diagnostic #03: I2C OLED Test");
    Serial.println("=======================================================\n");

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000); // 400 kHz Fast Mode
    scanI2CBus();

    Serial.println("\nInitializing U8g2 SSD1306 driver...");
    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(12, 28, "SSD1306 INITIALIZED");
    u8g2.drawStr(12, 44, "Rev 2.0 I2C Active");
    u8g2.sendBuffer();
    delay(1000);

    Serial.println("Starting 30-45 FPS animation loop...");
    lastFpsMillis = millis();
}

void loop() {
    // 1. Move bouncing ball
    ballX += ballDX;
    ballY += ballDY;

    if (ballX <= 3 || ballX >= 124) ballDX = -ballDX;
    if (ballY <= 17 || ballY >= 60) ballDY = -ballDY;

    // 2. FPS Calculation
    frameCount++;
    if (millis() - lastFpsMillis >= 1000) {
        currentFps = frameCount;
        frameCount = 0;
        lastFpsMillis = millis();
    }

    // 3. Render frame
    u8g2.clearBuffer();

    // Top Header Banner
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(2, 10, "I2C OLED DIAGNOSTIC");
    char fpsBuf[16];
    std::snprintf(fpsBuf, sizeof(fpsBuf), "%u FPS", currentFps);
    u8g2.drawStr(90, 10, fpsBuf);
    u8g2.drawHLine(0, 13, 128);

    // Frame border
    u8g2.drawFrame(0, 15, 128, 49);

    // Draw Pin assignments
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(4, 23, "SDA: GPIO 8 | SCL: GPIO 9");

    // Draw bouncing circle
    u8g2.drawDisc(ballX, ballY, 3);

    u8g2.sendBuffer();
}
