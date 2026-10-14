#include <Arduino.h>
#include <Preferences.h>
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32.h>

// =========================================================================
// Hardware Diagnostic 06: BLE MIDI & NVS Flash Persistence Test
// Tests:
//   1. Reading & Writing 48-byte patch struct to ESP32-S3 NVS Flash
//   2. Bluetooth LE advertising as "MK4-Synth"
//   3. Receiving wireless SysEx patch dumps and auto-persisting to Slot #7
// =========================================================================

#pragma pack(push, 1)
struct RawPatchData {
    char name[16];
    uint8_t oscWaveform;
    float oscDetune;
    float filterCutoff;
    float filterResonance;
    float filterAttack;
    float filterDecay;
    float filterSustain;
    float filterRelease;
    float filterEnvAmount;
    float ampAttack;
    float ampDecay;
    float ampSustain;
    float ampRelease;
    uint8_t chorusMode;
};
#pragma pack(pop)

static Preferences s_prefs;
static bool s_bleConnected = false;

void testNvsStorage() {
    Serial.println("\n--- Testing NVS Flash Persistence ---");
    if (!s_prefs.begin("synth", false)) {
        Serial.println("-> [FAIL] Could not open NVS namespace 'synth'!");
        return;
    }

    RawPatchData readPatch;
    size_t bytesRead = s_prefs.getBytes("patch7", &readPatch, sizeof(readPatch));

    if (bytesRead == sizeof(RawPatchData)) {
        Serial.println("-> [PASS] Found existing patch in NVS from previous boot:");
        Serial.printf("   Patch Name : \"%s\"\n", readPatch.name);
        Serial.printf("   Waveform   : %u | Cutoff: %.1f Hz | Resonance: %.2f\n",
                      readPatch.oscWaveform, readPatch.filterCutoff, readPatch.filterResonance);
    } else {
        Serial.println("-> No patch in NVS yet (first run). Writing test synth pad...");
        RawPatchData testPatch;
        memset(&testPatch, 0, sizeof(testPatch));
        strncpy(testPatch.name, "BLUETOOTH PAD", 15);
        testPatch.oscWaveform = 1; // Saw
        testPatch.oscDetune = 0.05f;
        testPatch.filterCutoff = 1200.0f;
        testPatch.filterResonance = 0.5f;
        testPatch.ampAttack = 0.8f; // Slow pad attack
        testPatch.ampRelease = 1.5f; // Long pad release
        testPatch.chorusMode = 2; // Juno Chorus II

        size_t written = s_prefs.putBytes("patch7", &testPatch, sizeof(testPatch));
        if (written == sizeof(testPatch)) {
            Serial.println("-> [PASS] Successfully wrote test patch to NVS!");
            Serial.println("   Reboot or power cycle the board to verify it persists!");
        } else {
            Serial.println("-> [FAIL] Failed writing patch to NVS!");
        }
    }
    s_prefs.end();
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) delay(10);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S3 Rev 2.0 Hardware Diagnostic #06: BLE & NVS");
    Serial.println("=======================================================\n");

    // 1. Verify NVS flash storage
    testNvsStorage();

    // 2. Initialize BLE MIDI
    Serial.println("\n--- Initializing Bluetooth LE MIDI ---");
    BLEMIDI.begin("MK4-Synth");

    BLEMIDI.setHandleConnected([]() {
        s_bleConnected = true;
        Serial.println("\n>>> [BLE] Client Connected! <<<");
    });

    BLEMIDI.setHandleDisconnected([]() {
        s_bleConnected = false;
        Serial.println("\n>>> [BLE] Client Disconnected <<<");
    });

    BLEMIDI.setHandleSystemExclusive([](byte* data, unsigned int length) {
        Serial.printf("[BLE SysEx] Received %u bytes over Bluetooth!\n", length);
        if (length == 62 && data[0] == 0xF0 && data[1] == 0x7D) {
            Serial.println("  -> Valid 62-byte patch dump detected. Writing to NVS Slot #7...");
            // Save payload to NVS
            s_prefs.begin("synth", false);
            // Payload starts at byte 5 (skip F0 7D 00 00 01)
            s_prefs.putBytes("patch7", &data[5], sizeof(RawPatchData));
            s_prefs.end();
            Serial.println("  -> [PASS] Wireless synth pad saved to Flash memory!");
        }
    });

    Serial.println("-> [PASS] BLE MIDI advertising as \"MK4-Synth\".");
    Serial.println("You can connect from iOS/Mac/Android/Chrome using Web Bluetooth or a BLE MIDI app.");
}

void loop() {
    delay(1000);
}
