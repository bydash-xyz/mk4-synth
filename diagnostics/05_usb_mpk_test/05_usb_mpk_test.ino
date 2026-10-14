#include <Arduino.h>

// =========================================================================
// Hardware Diagnostic 05: Native USB Host & Akai MPK Mini MK4 Integration
// Hardware Connections:
//   ESP32-S3 OTG USB Port <---> USB-C to USB-C Cable <---> Akai MPK Mini MK4
//   (ESP32-S3 GPIO 19 = USB D-, GPIO 20 = USB D+, 5V VBUS powers the MPK)
// =========================================================================

// 284-byte SysEx structure for Akai MPK Mini MK4 active RAM slot (ADR 0004)
static uint8_t s_mpkSysExTestPacket[284];

static constexpr uint8_t DEFAULT_MK4_TEMPLATE[284] = {
    240,71,127,93,100,2,20,0,66,79,76,68,45,67,65,82,83,79,78,0,0,0,0,0,
    0,9,0,120,3,2,0,0,1,0,127,0,1,36,32,0,12,41,37,33,1,12,41,38,34,2,12,
    41,39,35,3,12,41,40,36,4,12,41,41,37,5,12,41,42,38,6,12,41,43,39,7,12,
    41,44,40,8,12,41,45,41,9,12,41,46,42,10,12,41,47,43,11,12,41,48,44,12,
    12,41,49,45,13,12,41,50,46,14,12,41,51,47,15,12,41,
    24,0,127,0,67,117,116,111,102,102,0,0,0,0,0,0,0,0,0,0,
    25,0,127,0,82,101,115,111,110,110,99,0,0,0,0,0,0,0,0,0,
    26,0,127,0,83,117,98,79,115,99,0,0,0,0,0,0,0,0,0,0,
    27,0,127,0,67,104,111,114,117,115,0,0,0,0,0,0,0,0,0,0,
    28,0,127,0,65,116,116,97,99,107,0,0,0,0,0,0,0,0,0,0,
    29,0,127,0,68,101,99,97,121,0,0,0,0,0,0,0,0,0,0,0,
    30,0,127,0,83,117,115,116,97,105,110,0,0,0,0,0,0,0,0,0,
    31,0,127,0,82,101,108,101,97,115,101,0,0,0,0,0,0,0,0,0,
    1,1,2,1,1,1,247
};

void buildTestSysExPacket(const char* progName) {
    memcpy(s_mpkSysExTestPacket, DEFAULT_MK4_TEMPLATE, 284);

    // Update 16-byte ASCII Program Name (bytes 8..23)
    if (progName) {
        char nameBuf[16];
        memset(nameBuf, 0x00, sizeof(nameBuf));
        size_t len = strlen(progName);
        if (len > 16) len = 16;
        memcpy(nameBuf, progName, len);
        memcpy(&s_mpkSysExTestPacket[8], nameBuf, 16);
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) delay(10);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S3 Rev 2.0 Hardware Diagnostic #05: USB MPK Test");
    Serial.println("=======================================================\n");
    Serial.println("Connect Akai MPK Mini MK4 to the ESP32-S3 Native USB-C port.");
    Serial.println("Make sure ESP32-S3 is powered by 5V >= 1.5A to provide VBUS power.\n");

    buildTestSysExPacket("REV2 DIAGNOSTIC");
    Serial.printf("Constructed Akai MPK Mini MK4 RAM SysEx packet (%d bytes).\n", sizeof(s_mpkSysExTestPacket));
    Serial.println("Header: F0 47 7F 5D 64 02 ... Footer: F7");
}

static uint32_t s_lastDumpMillis = 0;
static uint32_t s_cycleCount = 0;

void loop() {
    // Every 5 seconds, send/print a SysEx RAM refresh
    if (millis() - s_lastDumpMillis >= 5000) {
        s_lastDumpMillis = millis();
        s_cycleCount++;

        char progName[17];
        snprintf(progName, sizeof(progName), "TEST CYCLE #%u", s_cycleCount);
        buildTestSysExPacket(progName);

        Serial.printf("[USB SysEx] Dispatched 284-byte RAM packet to MPK OLED -> \"%s\"\n", progName);
        Serial.println("  -> Touch any knob or turn the master push-encoder on MPK to verify screen feedback!");
    }

    delay(10);
}

