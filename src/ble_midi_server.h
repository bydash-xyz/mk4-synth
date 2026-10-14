#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include "SynthEngine.h"
#include "Presets.h"
#include "SysExProtocol.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <Preferences.h>
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32.h>
#include <esp_bt.h>

// Same objects BLEMIDI_CREATE_INSTANCE("MK4-Synth", MIDI) declares, but with a 512-byte SysEx limit:
// the library default (128) would split a PatchDumpV2 into fragments that no handler can decode.
struct SynthBleMidiSettings : public BLEMIDI_NAMESPACE::MySettings {
    static const unsigned SysExMaxSize = Synth::MAX_SYSEX_MESSAGE_SIZE;
};
BLEMIDI_NAMESPACE::BLEMIDI_Transport<BLEMIDI_NAMESPACE::BLEMIDI_ESP32> BLEMIDI("MK4-Synth");
MIDI_NAMESPACE::MidiInterface<BLEMIDI_NAMESPACE::BLEMIDI_Transport<BLEMIDI_NAMESPACE::BLEMIDI_ESP32>,
                              SynthBleMidiSettings> MIDI(BLEMIDI);
#elif defined(MOCK_HAL)
#include "Arduino.h"
#include "Preferences.h"
#endif

namespace HardwareDriver {

class BleMidiServer;
inline BleMidiServer* s_bleServerInstance = nullptr;

// =========================================================================
// BLE SysEx Patch Server & NVS Flash Persistence Handler
// Only handles BLE-based patch updates and NVS persistence.
// Performance MIDI input (Note On/Off, CC, Pitch Bend) is handled via USB Host MPK.
// =========================================================================
class BleMidiServer {
public:
    static constexpr const char* DEVICE_NAME = "MK4-Synth";
    static constexpr const char* NVS_NAMESPACE = "synth";

    BleMidiServer() : synthEngine_(nullptr), isConnected_(false) {}

    // Returns false if BLE or NVS failed to come up (see isBleOk() / isNvsOk())
    bool init(Synth::SynthEngine& engine) {
        synthEngine_ = &engine;

        // Probe NVS once at boot (opening the namespace read-write creates it on first boot)
        {
            Preferences probe;
            nvsOk_ = probe.begin(NVS_NAMESPACE, false);
            if (nvsOk_) probe.end();
        }

        // Register NVS storage persistence callbacks with SynthEngine. v2 patch bodies live under
        // q_<slot>; legacy 48-byte v1 blobs under p_<slot> are only read, and left in place so older
        // firmware still finds them after a downgrade. h_<slot> fingerprints the p_<slot> blob present
        // when q_<slot> was written: if older firmware has since saved a new p_<slot>, that one wins.
        synthEngine_->setStorageCallbacks(
            [this](uint8_t slot, const uint8_t* data, size_t size) -> bool {
                Preferences prefs;
                if (!prefs.begin(NVS_NAMESPACE, false)) {
                    Serial.println("[NVS Error] Failed to open NVS for writing");
                    nvsOk_ = false;
                    return false;
                }
                char key[16];
                std::snprintf(key, sizeof(key), "q_%u", slot);
                size_t written = prefs.putBytes(key, data, size);
                std::snprintf(key, sizeof(key), "h_%u", slot);
                prefs.putUInt(key, legacyFingerprint(prefs, slot));
                prefs.end();
                return (written == size);
            },
            [](uint8_t slot, uint8_t* outData, size_t maxSize) -> size_t {
                Preferences prefs;
                if (!prefs.begin(NVS_NAMESPACE, true)) {
                    return 0;
                }
                char key[16];
                std::snprintf(key, sizeof(key), "h_%u", slot);
                const bool legacyNewer = prefs.getUInt(key, 0) != legacyFingerprint(prefs, slot);
                std::snprintf(key, sizeof(key), "q_%u", slot);
                const size_t len = prefs.getBytesLength(key);
                const size_t readBytes = (!legacyNewer && len > 0 && len <= maxSize) ? prefs.getBytes(key, outData, maxSize) : 0;
                prefs.end();
                return readBytes;
            },
            [](uint8_t slot, uint8_t* outData, size_t size) -> bool {
                Preferences prefs;
                if (!prefs.begin(NVS_NAMESPACE, true)) {
                    return false;
                }
                char key[16];
                std::snprintf(key, sizeof(key), "p_%u", slot);
                size_t readBytes = prefs.getBytesLength(key) == size ? prefs.getBytes(key, outData, size) : 0;
                prefs.end();
                return (readBytes == size);
            }
        );

        // Restore saved patches from NVS for all user slots (#24..#32)
        synthEngine_->loadAllUserPresetsFromStorage();

#if defined(ARDUINO_ARCH_ESP32)
        s_bleServerInstance = this;
        // Initialize BLE transport & MIDI interface for SysEx patch management
        MIDI.begin();
        BLEMIDI.setHandleConnected([]() {
            if (s_bleServerInstance) {
                s_bleServerInstance->isConnected_ = true;
                Serial.println("[BLE] Central device connected for patch management");
            }
        });
        BLEMIDI.setHandleDisconnected([]() {
            if (s_bleServerInstance) {
                s_bleServerInstance->isConnected_ = false;
                Serial.println("[BLE] Central device disconnected");
            }
        });
        // Dedicated BLE SysEx handler for wireless patch dumps & updates
        MIDI.setHandleSystemExclusive([](byte* data, unsigned int length) {
            if (s_bleServerInstance) s_bleServerInstance->handleSysExReceived(data, length);
        });
        bleOk_ = esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
#endif
        return bleOk_ && nvsOk_;
    }

    bool isBleOk() const { return bleOk_; }
    // False if NVS could not be opened at boot or a later patch write could not open it
    bool isNvsOk() const { return nvsOk_; }

    void poll() {
#if defined(ARDUINO_ARCH_ESP32)
        while (MIDI.read()) {}
#endif
    }

    bool isConnected() const { return isConnected_; }

    // Direct handler for incoming SysEx (used by BLE callback & test harness)
    bool handleSysExReceived(const uint8_t* data, size_t length) {
        if (!data || length < 4 || !synthEngine_) return false;

        // The reply comes back with the call (not via a shared callback), so replies to SysEx handled on
        // other tasks (USB) never go out over BLE. It is sent after handleSysEx has released the engine lock.
        std::vector<uint8_t> reply;
        synthEngine_->handleSysEx(data, length, reply);
        sendReply(reply);
        const auto& resp = synthEngine_->getLastSysExResponse();
        if (!resp.empty() && resp.size() >= 7) {
            // Check if handshake status (byte 5) is AckSuccess
            return (resp[5] == static_cast<uint8_t>(Synth::HandshakeStatus::AckSuccess));
        }
        return false;
    }

    // Last reply sent back to the BLE central (test hook)
    const std::vector<uint8_t>& getLastSentResponse() const { return lastSentResponse_; }

private:
    void sendReply(std::vector<uint8_t>& reply) {
        if (reply.empty()) return;
#if defined(ARDUINO_ARCH_ESP32)
        if (isConnected_) {
            MIDI.sendSysEx(static_cast<unsigned>(reply.size()), reply.data(), true);
        }
#endif
        lastSentResponse_.swap(reply);
    }

    // FNV-1a of the legacy p_<slot> blob (0 if absent); NVS must already be open
    static uint32_t legacyFingerprint(Preferences& prefs, uint8_t slot) {
        char key[16];
        std::snprintf(key, sizeof(key), "p_%u", slot);
        uint8_t blob[sizeof(Synth::RawPatchData)];
        if (prefs.getBytesLength(key) != sizeof(blob) || prefs.getBytes(key, blob, sizeof(blob)) != sizeof(blob)) {
            return 0;
        }
        uint32_t h = 2166136261u;
        for (uint8_t b : blob) h = (h ^ b) * 16777619u;
        return h;
    }

    Synth::SynthEngine* synthEngine_;
    std::vector<uint8_t> lastSentResponse_;
    bool isConnected_;
    bool bleOk_{true};
    volatile bool nvsOk_{true}; // also cleared by the storage write callback (SysEx handling task)
};

} // namespace HardwareDriver
