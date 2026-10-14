#include <Arduino.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

// =========================================================================
// Hardware Diagnostic 01: System Health, Clock, Flash & PSRAM
// Hardware Target: ESP32-S3-DevKitC-1 (N16R8)
// =========================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(10);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S3 Rev 2.0 Hardware Diagnostic #01: System Health");
    Serial.println("=======================================================\n");

    // 1. CPU & Chip Information
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    Serial.printf("Chip Model       : ESP32-S3 (Revision %d)\n", chip_info.revision);
    Serial.printf("CPU Cores        : %d Cores @ %d MHz\n", chip_info.cores, getCpuFrequencyMhz());
    Serial.printf("Silicon Features : %s%s\n",
                  (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi 802.11b/g/n " : "",
                  (chip_info.features & CHIP_FEATURE_BLE) ? "Bluetooth 5.0 LE" : "");

    // 2. SPI Flash Verification (Target: 16 MB)
    uint32_t flashSize = 0;
    esp_flash_get_size(NULL, &flashSize);
    uint32_t flashMb = flashSize / (1024 * 1024);
    Serial.printf("SPI Flash Size   : %u MB (%u bytes)\n", flashMb, flashSize);
    if (flashMb >= 16) {
        Serial.println("  -> [PASS] 16 MB SPI Flash detected correctly.");
    } else {
        Serial.printf("  -> [FAIL] Expected 16 MB Flash, found %u MB. Check board_build.flash_size!\n", flashMb);
    }

    // 3. Octal PSRAM Verification (Target: 8 MB)
    Serial.println("\nChecking Octal PSRAM (N16R8)...");
    if (psramInit()) {
        size_t psramSize = ESP.getPsramSize();
        size_t freePsram = ESP.getFreePsram();
        size_t psramMb = psramSize / (1024 * 1024);
        Serial.printf("PSRAM Total Size : %u MB (%u bytes)\n", (uint32_t)psramMb, (uint32_t)psramSize);
        Serial.printf("PSRAM Free Space : %u MB (%u bytes)\n", (uint32_t)(freePsram / (1024 * 1024)), (uint32_t)freePsram);

        // Perform memory read/write test in PSRAM
        constexpr size_t TEST_SIZE = 1024 * 1024; // 1 MB buffer
        Serial.printf("Testing 1 MB read/write buffer in PSRAM... ");
        uint32_t* testBuf = (uint32_t*)ps_malloc(TEST_SIZE);
        if (testBuf != nullptr) {
            bool passed = true;
            for (size_t i = 0; i < TEST_SIZE / sizeof(uint32_t); ++i) {
                testBuf[i] = 0xAA550000 | (i & 0xFFFF);
            }
            for (size_t i = 0; i < TEST_SIZE / sizeof(uint32_t); ++i) {
                if (testBuf[i] != (0xAA550000 | (i & 0xFFFF))) {
                    passed = false;
                    break;
                }
            }
            free(testBuf);

            if (passed) {
                Serial.println("[PASS] 1 MB PSRAM Integrity Verified!");
            } else {
                Serial.println("[FAIL] PSRAM read/write comparison mismatch!");
            }
        } else {
            Serial.println("[FAIL] Could not allocate 1 MB in PSRAM!");
        }
    } else {
        Serial.println("  -> [FAIL] PSRAM not detected or failed to initialize!");
        Serial.println("     Make sure 'qio_opi' memory type and -DBOARD_HAS_PSRAM are enabled.");
    }

    // 4. Internal SRAM Verification
    Serial.printf("\nInternal Heap Free : %u KB\n", ESP.getFreeHeap() / 1024);
    Serial.printf("Minimum Free Heap  : %u KB\n", ESP.getMinFreeHeap() / 1024);

    Serial.println("\n-------------------------------------------------------");
    Serial.println("Diagnostic #01 complete. System heartbeat running below...");
    Serial.println("-------------------------------------------------------");
}

static uint32_t counter = 0;
void loop() {
    delay(2000);
    counter += 2;
    Serial.printf("[Uptime %us] CPU0 Temp: %.1f°C | Free Heap: %u KB | Free PSRAM: %u KB\n",
                  counter,
                  temperatureRead(),
                  ESP.getFreeHeap() / 1024,
                  ESP.getFreePsram() / 1024);
}
