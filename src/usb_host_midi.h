#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include "SynthEngine.h"
#include "Presets.h"
#include "knob_page.h"
#include "MpkMiniMk4ControlSurface.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <usb/usb_host.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#elif defined(MOCK_HAL)
#include "Arduino.h"
#include "usb/usb_host.h"
#else
#include <chrono>
#include <cstdio>
inline uint32_t millis() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
}
struct DesktopSerial {
    template<typename... Args>
    void printf(const char* fmt, Args&&... args) {
        std::printf(fmt, std::forward<Args>(args)...);
    }
    void println(const char* str = "") {
        std::printf("%s\n", str);
    }
};
static DesktopSerial Serial;
#endif

// Per-event MIDI logging. Off by default: these run in the USB client task, and Serial is the
// 115200-baud UART, so each line blocks USB polling for several ms. Build with -DMIDI_VERBOSE_LOG to trace input.
#if defined(MIDI_VERBOSE_LOG)
#define MIDI_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define MIDI_LOG(...) ((void)0)
#endif

namespace HardwareDriver {

// =========================================================================
// Native USB Host MIDI Driver for Akai MPK Mini MK4 (Core 0)
// =========================================================================
class UsbHostMidi {
public:
    // MK4 control CCs, as named in the tests and the simulator
    static constexpr uint8_t CC_MASTER_ENCODER    = Synth::CC_HW_CUSTOM_ENCODER;
    static constexpr uint8_t CC_MASTER_PUSH       = Synth::CC_HW_CUSTOM_PUSH;
    static constexpr uint8_t CC_BANK_LEFT         = Synth::CC_HW_CUSTOM_BANK_LEFT;
    static constexpr uint8_t CC_BANK_RIGHT        = Synth::CC_HW_CUSTOM_BANK_RIGHT;
    static constexpr uint8_t CC_SHIFT_BUTTON      = Synth::CC_HW_SHIFT_BUTTON;     // CC 17 physical hardware
    static constexpr uint8_t CC_METRONOME_BTN     = Synth::CC_HW_CUSTOM_METRO_BTN;
    static constexpr uint8_t CC_TAP_TEMPO_BTN     = Synth::CC_HW_CUSTOM_TAP_BTN;
    static constexpr uint8_t CC_MPK4_MINUS        = Synth::CC_HW_PRESET_PREV;
    static constexpr uint8_t CC_MPK4_PLUS         = Synth::CC_HW_PRESET_NEXT;
    static constexpr uint8_t CC_MPK4_ENCODER      = Synth::CC_HW_NAV_ENCODER;
    static constexpr uint8_t CC_MPK4_BANK_AB      = Synth::CC_HW_BANK_AB_BUTTON; // CC 12 on physical hardware

    using MidiUiEvent = MpkMiniMk4::UiEvent;

    using SysExSendCallback = std::function<void(const std::vector<uint8_t>&)>;
    using PresetChangeCallback = std::function<void(uint8_t index, const char* name)>;
    using PageChangeCallback = std::function<void(KnobPage page)>;
    using StatusHudCallback = std::function<void(const char* label, uint8_t value, const char* valueText)>;
    using AnimationStepCallback = MpkMiniMk4::ControlSurface::AnimationStepCallback;

    UsbHostMidi() = default;

    ~UsbHostMidi() {
#if defined(ARDUINO_ARCH_ESP32)
        if (txMutex_) {
            vSemaphoreDelete(txMutex_);
            txMutex_ = nullptr;
        }
#endif
    }

    bool init(Synth::SynthEngine& engine) {
        synthEngine_ = &engine;
        controlSurface_.init(engine);
#if defined(ARDUINO_ARCH_ESP32)
        controlSurface_.setScreenTakeoverEnabled(false);
#endif

        controlSurface_.setOnStatusHud([this](const char* label, uint8_t val, const char* text) {
            if (statusHudCb_) statusHudCb_(label, val, text);
        });
        controlSurface_.setOnPresetChanged([this](uint8_t idx, const char* name) {
            if (presetChangedCb_) presetChangedCb_(idx, name);
        });

        auto defaultSend = [this](const uint8_t* data, size_t len) {
            std::vector<uint8_t> pkt(data, data + len);
            if (sysExSendCb_) sysExSendCb_(pkt);
#if defined(ARDUINO_ARCH_ESP32)
            sendUsbMidiSysExInternal(pkt, cableForSysEx(pkt));
#endif
        };
        controlSurface_.setPluginPortSender(defaultSend);
        controlSurface_.setSoftwareControlPortSender(defaultSend);

#if defined(ARDUINO_ARCH_ESP32)
        if (!txMutex_) {
            txMutex_ = xSemaphoreCreateMutex();
        }
        ESP_LOGI("UsbHostMidi", "[USB Host] Initializing native USB Host MIDI...");

        const usb_host_config_t host_config = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };
        esp_err_t err = usb_host_install(&host_config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE("UsbHostMidi", "usb_host_install failed: 0x%x", err);
            return false;
        }

        isTaskRunning_ = true;
        xTaskCreatePinnedToCore(hostTask, "usbHostLibTask", 4096, this, 5, &host_task_handle_, 0);

        const usb_host_client_config_t client_config = {
            .is_synchronous = false,
            .max_num_event_msg = 10,
            .async = {
                .client_event_callback = clientEventCb,
                .callback_arg = this,
            }
        };
        err = usb_host_client_register(&client_config, &client_handle_);
        if (err != ESP_OK) {
            ESP_LOGE("UsbHostMidi", "usb_host_client_register failed: 0x%x", err);
            return false;
        }

        xTaskCreatePinnedToCore(clientTask, "usbClientTask", 4096, this, 2, &client_task_handle_, 0);
#elif defined(MOCK_HAL)
        // Mock HAL initialization
#endif
        return true;
    }

    void setOnSysExOut(SysExSendCallback cb) {
        sysExSendCb_ = cb;
#if defined(ARDUINO_ARCH_ESP32)
        auto sendFn = [this, cb](const uint8_t* data, size_t len) {
            std::vector<uint8_t> pkt(data, data + len);
            if (cb) cb(pkt);
            sendUsbMidiSysExInternal(pkt, cableForSysEx(pkt));
        };
#else
        auto sendFn = [cb](const uint8_t* data, size_t len) {
            std::vector<uint8_t> pkt(data, data + len);
            if (cb) cb(pkt);
        };
#endif
        controlSurface_.setPluginPortSender(sendFn);
        controlSurface_.setSoftwareControlPortSender(sendFn);
    }


    void setOnPresetChanged(PresetChangeCallback cb) {
        presetChangedCb_ = cb;
    }

    void setOnPageChanged(PageChangeCallback cb) {
        pageChangedCb_ = cb;
    }

    // SHIFT + '-' / '+' steps the OLED animation (-1 / +1)
    void setOnAnimationStep(AnimationStepCallback cb) {
        controlSurface_.setOnAnimationStep(std::move(cb));
    }

    // Semitone shift (octave + transpose) the MPK applies to keybed notes, as it reports it
    int getKeybedNoteOffset() const { return controlSurface_.getKeybedNoteOffset(); }

    void setOnStatusHud(StatusHudCallback cb) {
        statusHudCb_ = cb;
    }

    // Call from Core 0 main loop (~1 ms polling interval for HUD timers & lease keepalive)
    void poll() {
#if defined(ARDUINO_ARCH_ESP32)
        uint32_t now = millis();
        if (isDeviceConnected_ && controlSurface_.getDisplayDriver().isScreenTakeoverEnabled() && (now - lastKeepaliveTime_ >= 1500)) {
            lastKeepaliveTime_ = now;
            controlSurface_.sendKeepalive();
        }
        // Poll the MPK's octave/transpose: OCT+/- may not emit a 0x6A notification
        if (isDeviceConnected_ && (now - lastKeybedPollTime_ >= 300)) {
            lastKeybedPollTime_ = now;
            controlSurface_.requestKeybedState();
        }
#endif
        tick();
    }

    // One MIDI message from the MK4. MpkMiniMk4::ControlSurface does the routing; this adds the
    // lost-note bookkeeping and tells the OLED when the knob page changes.
    MidiUiEvent processMidiMessage(uint8_t status, uint8_t data1, uint8_t data2) {
        if (!synthEngine_) return MidiUiEvent::NONE;
        const KnobPage pageBefore = currentKnobPage();
        controlSurface_.processMidiMessage(status, data1, data2);
        const MidiUiEvent event = controlSurface_.getLastUiEvent();
        if (event == MidiUiEvent::NOTE_ON) {
            MIDI_LOG("[MPK4 NoteOn] Note %u (Vel %u)\n", data1, data2);
            trackNoteOn(data1);
        } else if (event == MidiUiEvent::NOTE_OFF) {
            MIDI_LOG("[MPK4 NoteOff] Note %u\n", data1);
            trackNoteOff(data1);
        } else if (status < 0xF8 && (status & 0xF0) == 0xB0) {
            MIDI_LOG("[MPK4 CC] CC %u = %u\n", data1, data2);
        }
        if (currentKnobPage() != pageBefore) publishKnobPage();
        return event;
    }

    void selectPreset(uint8_t presetIndex) {
        const KnobPage pageBefore = currentKnobPage();
        controlSurface_.selectPreset(presetIndex);
        if (currentKnobPage() != pageBefore) publishKnobPage();
    }

    void stepPreset(int step) {
        const KnobPage pageBefore = currentKnobPage();
        controlSurface_.stepPreset(step);
        if (currentKnobPage() != pageBefore) publishKnobPage();
    }

    void setBankBLatched(bool latched) {
        controlSurface_.setBankBLatched(latched);
        publishKnobPage();
    }

    void setRhythmPageActive(bool active) {
        controlSurface_.setRhythmPageActive(active);
        publishKnobPage();
    }

    KnobPage currentKnobPage() const {
        if (controlSurface_.isRhythmPageActive()) return KnobPage::RHYTHM;
        return controlSurface_.isBankBActive() ? KnobPage::BANK_B : KnobPage::BANK_A;
    }

    uint8_t getCurrentPreset() const { return controlSurface_.getCurrentPreset(); }
    // Effective knob bank (BANK A/B button only; SHIFT never changes bank, it is the firmware fine-adjust modifier)
    bool isBankBActive() const { return controlSurface_.isBankBActive(); }
    bool isBankBLatched() const { return controlSurface_.isBankBLatched(); }
    bool isRhythmPageActive() const { return controlSurface_.isRhythmPageActive(); }
    bool isShiftHeld() const { return controlSurface_.isShiftButtonHeld(); }
    MidiUiEvent getLastUiEvent() const { return controlSurface_.getLastUiEvent(); }
    uint8_t getLastKnobIndex() const { return controlSurface_.getLastKnobIndex(); }
    uint8_t getLastKnobValue() const { return controlSurface_.getLastKnobValue(); }
    uint8_t getLastTargetCc() const { return controlSurface_.getLastTargetCc(); }

    MpkMiniMk4::ControlSurface& getControlSurface() { return controlSurface_; }
    const MpkMiniMk4::ControlSurface& getControlSurface() const { return controlSurface_; }

    // Processes inbound SysEx from controller (e.g. Heartbeat Ping CMD 0x19)
    bool processSysEx(const uint8_t* data, size_t len) {
        countMpkSysEx(data, len);
        return controlSurface_.processSysEx(data, len);
    }

    // MK4 enumerated and its MIDI interface claimed (read from loop(); written by the USB client task)
    bool isDeviceConnected() const { return isDeviceConnected_; }

    // Inbound Akai (0x47) SysEx messages so far: keybed-state replies to the 300 ms poll, heartbeat pings.
    // SystemStatus::updateMpkLink() treats a count that stops moving as "Plugin Port offline".
    uint32_t getMpkSysExCount() const { return mpkSysExCount_; }

    // Keybed note pairing: a note-on for a key already down means its note-off was lost (stuck note);
    // a note-off for a key that is up means its note-on was lost. Read from loop() for the heartbeat log.
    uint32_t getLostNoteOffCount() const { return lostNoteOffs_; }
    uint32_t getLostNoteOnCount() const { return lostNoteOns_; }

    // Lights a MK4 button LED: Note-On whose note is the button's CC, on the Plugin Port (the active port of the
    // PLUGIN program). Channel 1..7 = solid at 10/25/50/65/75/90/100 %; level 0 turns it off (RE doc §4.5).
    void setButtonLed(uint8_t buttonCc, uint8_t channel, bool on) {
#if defined(ARDUINO_ARCH_ESP32)
        sendUsbMidiNoteOnInternal(buttonCc, on ? 127 : 0, 2, channel);
#else
        (void)buttonCc; (void)channel; (void)on;
#endif
    }

    // Sends an explicit Screen Owner Keepalive Pong over Plugin Port
    bool sendKeepalive() {
        return controlSurface_.sendKeepalive();
    }

    // Periodic processing (e.g. parameter HUD auto-dismiss timer)
    void tick(uint32_t nowMs = 0) {
        controlSurface_.tick(nowMs);
    }

private:
    MpkMiniMk4::ControlSurface controlSurface_{};
    Synth::SynthEngine* synthEngine_{nullptr};
    volatile bool isDeviceConnected_{false};
    volatile uint32_t mpkSysExCount_{0};
    bool keyDown_[128]{};
    volatile uint32_t lostNoteOffs_{0};
    volatile uint32_t lostNoteOns_{0};
    void trackNoteOn(uint8_t note) {
        if (keyDown_[note & 0x7F]) lostNoteOffs_ = lostNoteOffs_ + 1;
        keyDown_[note & 0x7F] = true;
    }
    void trackNoteOff(uint8_t note) {
        if (!keyDown_[note & 0x7F]) lostNoteOns_ = lostNoteOns_ + 1;
        keyDown_[note & 0x7F] = false;
    }

    void publishKnobPage() {
        if (pageChangedCb_) pageChangedCb_(currentKnobPage());
    }

    uint32_t lastKeepaliveTime_{0};
    uint32_t lastKeybedPollTime_{0};
    SysExSendCallback sysExSendCb_;
    PresetChangeCallback presetChangedCb_;
    PageChangeCallback pageChangedCb_;
    StatusHudCallback statusHudCb_;

    uint8_t incomingSysExBuf_[256]{};
    size_t incomingSysExLen_{0};

    void dispatchInboundSysEx() {
        if (incomingSysExLen_ >= 4) {
#if defined(ARDUINO_ARCH_ESP32)
            // skip heartbeat pings and the periodic octave/transpose poll replies (0x6A)
            const bool isPing = incomingSysExLen_ == 8 && incomingSysExBuf_[4] == 0x19 && incomingSysExBuf_[6] == 0x00;
            if (!(incomingSysExLen_ >= 5 && (isPing || incomingSysExBuf_[4] == 0x6A))) {
                Serial.print("[USB SysEx in]");
                for (size_t i = 0; i < incomingSysExLen_ && i < 24; ++i) Serial.printf(" %02X", incomingSysExBuf_[i]);
                Serial.println();
            }
#endif
            // Process MPK Mini MK4 native protocol (e.g. Heartbeat Ping CMD 0x19)
            countMpkSysEx(incomingSysExBuf_, incomingSysExLen_);
            controlSurface_.processSysEx(incomingSysExBuf_, incomingSysExLen_);

            // Process mk4-synth patch protocol
            if (synthEngine_) {
                synthEngine_->handleSysEx(incomingSysExBuf_, incomingSysExLen_);
            }
        }
        incomingSysExLen_ = 0;
    }

    void countMpkSysEx(const uint8_t* data, size_t len) {
        if (data && len >= 2 && data[0] == 0xF0 && data[1] == 0x47) {
            mpkSysExCount_ = mpkSysExCount_ + 1; // single writer (USB client task)
        }
    }

    void feedUsbSysExPacket(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
        auto pushByte = [this](uint8_t b) {
            if (b == 0xF0) incomingSysExLen_ = 0;
            if (incomingSysExLen_ < sizeof(incomingSysExBuf_)) {
                incomingSysExBuf_[incomingSysExLen_++] = b;
            }
        };

        if (cin == 0x4) {
            pushByte(b1); pushByte(b2); pushByte(b3);
        } else if (cin == 0x5) {
            pushByte(b1);
            dispatchInboundSysEx();
        } else if (cin == 0x6) {
            pushByte(b1); pushByte(b2);
            dispatchInboundSysEx();
        } else if (cin == 0x7) {
            pushByte(b1); pushByte(b2); pushByte(b3);
            dispatchInboundSysEx();
        }
    }

#if defined(ARDUINO_ARCH_ESP32)
    // ESP32 USB Host Specific
    usb_host_client_handle_t client_handle_{nullptr};
    usb_device_handle_t device_handle_{nullptr};
    usb_transfer_t* transfer_in_{nullptr};
    usb_transfer_t* transfer_out_{nullptr};
    volatile bool transfer_out_busy_{false};
    std::vector<uint8_t> txQueue_{};
    SemaphoreHandle_t txMutex_{nullptr};
    uint8_t in_ep_{0};
    uint8_t out_ep_{0};
    uint16_t in_mps_{0};
    uint16_t out_mps_{0};
    uint8_t iface_num_{0};
    TaskHandle_t host_task_handle_{nullptr};
    TaskHandle_t client_task_handle_{nullptr};
    volatile bool isTaskRunning_{false};

    static void hostTask(void* arg) {
        while (true) {
            usb_host_lib_handle_events(portMAX_DELAY, nullptr);
        }
    }

    static void clientTask(void* arg) {
        UsbHostMidi* self = static_cast<UsbHostMidi*>(arg);
        while (self->isTaskRunning_) {
            if (self->client_handle_) {
                usb_host_client_handle_events(self->client_handle_, pdMS_TO_TICKS(5));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        vTaskDelete(nullptr);
    }

    static void clientEventCb(const usb_host_client_event_msg_t* event_msg, void* arg) {
        UsbHostMidi* self = static_cast<UsbHostMidi*>(arg);
        if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
            self->handleNewDevice(event_msg->new_dev.address);
        } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
            self->handleDeviceGone();
        }
    }

    void pumpTxQueueLocked() {
        if (!isDeviceConnected_ || !transfer_out_ || transfer_out_busy_ || txQueue_.empty()) {
            return;
        }

        // Find boundary of first message (CIN 0x5, 0x6, 0x7, or non-SysEx 0x8..0xE), up to 64 bytes
        size_t bytesToSend = 0;
        for (size_t i = 0; i + 4 <= txQueue_.size(); i += 4) {
            bytesToSend += 4;
            uint8_t cin = txQueue_[i] & 0x0F;
            if (cin != 0x04 || bytesToSend >= 64) {
                break;
            }
        }

        if (bytesToSend == 0) return;

        std::memcpy(transfer_out_->data_buffer, txQueue_.data(), bytesToSend);
        txQueue_.erase(txQueue_.begin(), txQueue_.begin() + bytesToSend);

        transfer_out_->num_bytes = bytesToSend;
        transfer_out_busy_ = true;

        esp_err_t err = usb_host_transfer_submit(transfer_out_);
        if (err != ESP_OK) {
            transfer_out_busy_ = false;
            ESP_LOGE("UsbHostMidi", "usb_host_transfer_submit OUT failed: 0x%x", err);
        }
    }

    // MK4 USB-MIDI cable map (firmware descriptors): 0 = MIDI Port, 1 = DAW Port, 2 = Plugin Port,
    // 3 = Software Control Port, 4 = DIN.
    // MK4 firmware accepts 0x3A (hardware Bank A/B state + button LED) only on cable 2 (Plugin Port; hardware-verified).
    // On cable 1 it is silently dropped, leaving the LED at its power-on state (red = Bank B).
    // Screen commands (0x10 text, 0x11/0x14 line format, 0x15 clear, 0x19 heartbeat, 0x1C take/release) also go to
    // cable 2: the MK4 picks the line buffer by arrival cable and renders the Plugin buffer for owner type 1
    // (hardware finding: on cable 1 the lines land in the DAW buffer and the screen stays black).
    // 0x71 pixel blits work on any cable (hardware-verified) but travel with the other screen commands so they stay
    // ordered after the line writes they draw on top of.
    // All other SysEx (knob seeding etc.) stay on cable 1, unchanged.
    // A full screen claim (lines + 0x71 strip fill + host-drawn value) queues ~8 KB of USB-MIDI packets. A message
    // is dropped whole when the queue is over this limit, which would desync the host-drawn value from the screen.
    static constexpr size_t TX_QUEUE_MAX_BYTES = 16384;

    static uint8_t cableForSysEx(const std::vector<uint8_t>& sysex) {
        if (sysex.size() <= 4) return 1;
        switch (sysex[4]) {
            case 0x3A: case 0x10: case 0x11: case 0x14: case 0x15: case 0x19: case 0x1C: case 0x71:
                return 2;
            default:
                return 1;
        }
    }

    void sendUsbMidiSysExInternal(const std::vector<uint8_t>& sysex, uint8_t cable = 1) {
        if (!isDeviceConnected_ || sysex.empty()) return;

        size_t sysexLen = sysex.size();
        size_t srcIdx = 0;
        uint8_t cableNibble = (cable & 0x0F) << 4;

        if (txMutex_) xSemaphoreTake(txMutex_, portMAX_DELAY);

        if (txQueue_.size() < TX_QUEUE_MAX_BYTES) {
            while (srcIdx < sysexLen) {
                size_t remaining = sysexLen - srcIdx;
                if (remaining > 3) {
                    txQueue_.push_back(cableNibble | 0x04);
                    txQueue_.push_back(sysex[srcIdx++]);
                    txQueue_.push_back(sysex[srcIdx++]);
                    txQueue_.push_back(sysex[srcIdx++]);
                } else if (remaining == 3) {
                    txQueue_.push_back(cableNibble | 0x07);
                    txQueue_.push_back(sysex[srcIdx++]);
                    txQueue_.push_back(sysex[srcIdx++]);
                    txQueue_.push_back(sysex[srcIdx++]);
                } else if (remaining == 2) {
                    txQueue_.push_back(cableNibble | 0x06);
                    txQueue_.push_back(sysex[srcIdx++]);
                    txQueue_.push_back(sysex[srcIdx++]);
                    txQueue_.push_back(0x00);
                } else if (remaining == 1) {
                    txQueue_.push_back(cableNibble | 0x05);
                    txQueue_.push_back(sysex[srcIdx++]);
                    txQueue_.push_back(0x00);
                    txQueue_.push_back(0x00);
                }
            }
            pumpTxQueueLocked();
        }

        if (txMutex_) xSemaphoreGive(txMutex_);
    }

    void sendUsbMidiNoteOnInternal(uint8_t note, uint8_t velocity, uint8_t cable, uint8_t channel) {
        if (!isDeviceConnected_) return;
        uint8_t cableNibble = (cable & 0x0F) << 4;
        if (txMutex_) xSemaphoreTake(txMutex_, portMAX_DELAY);
        if (txQueue_.size() < TX_QUEUE_MAX_BYTES) {
            txQueue_.push_back(cableNibble | 0x09);
            txQueue_.push_back(static_cast<uint8_t>(0x90 | (channel & 0x0F)));
            txQueue_.push_back(static_cast<uint8_t>(note & 0x7F));
            txQueue_.push_back(static_cast<uint8_t>(velocity & 0x7F));
            pumpTxQueueLocked();
        }
        if (txMutex_) xSemaphoreGive(txMutex_);
    }

    void handleNewDevice(uint8_t address) {
        esp_err_t err = usb_host_device_open(client_handle_, address, &device_handle_);
        if (err != ESP_OK) {
            ESP_LOGE("UsbHostMidi", "Failed to open device at address %d", address);
            return;
        }

        const usb_device_desc_t* dev_desc;
        const usb_config_desc_t* config_desc;

        usb_host_get_device_descriptor(device_handle_, &dev_desc);
        ESP_LOGI("UsbHostMidi", "Device opened. VID: 0x%04X, PID: 0x%04X", dev_desc->idVendor, dev_desc->idProduct);

        err = usb_host_get_active_config_descriptor(device_handle_, &config_desc);
        if (err != ESP_OK) {
            ESP_LOGE("UsbHostMidi", "Failed to get active config descriptor");
            return;
        }

        // Walk descriptors to find MIDI interface
        const uint8_t* p = reinterpret_cast<const uint8_t*>(config_desc) + config_desc->bLength;
        const uint8_t* end = reinterpret_cast<const uint8_t*>(config_desc) + config_desc->wTotalLength;

        bool found_midi = false;
        
        while (p < end) {
            uint8_t desc_len = p[0];
            uint8_t desc_type = p[1];
            
            if (desc_type == USB_W_VALUE_DT_INTERFACE) {
                const usb_intf_desc_t* intf = (const usb_intf_desc_t*)p;
                if (intf->bInterfaceClass == 0x01 && intf->bInterfaceSubClass == 0x03) {
                    found_midi = true;
                    iface_num_ = intf->bInterfaceNumber;
                    ESP_LOGI("UsbHostMidi", "Found Audio/MIDI Streaming Interface: %d", iface_num_);
                } else {
                    found_midi = false;
                }
            } else if (desc_type == USB_W_VALUE_DT_ENDPOINT && found_midi) {
                const usb_ep_desc_t* ep = (const usb_ep_desc_t*)p;
                Serial.printf("[USB Desc] Endpoint: 0x%02X, attr: 0x%02X, mps: %u\n", ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize);
                if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
                    if (ep->bEndpointAddress & 0x80) {
                        in_ep_ = ep->bEndpointAddress;
                        in_mps_ = ep->wMaxPacketSize;
                        ESP_LOGI("UsbHostMidi", "Found Bulk IN Endpoint: 0x%02X", in_ep_);
                    } else {
                        out_ep_ = ep->bEndpointAddress;
                        out_mps_ = ep->wMaxPacketSize;
                        ESP_LOGI("UsbHostMidi", "Found Bulk OUT Endpoint: 0x%02X", out_ep_);
                    }
                }
            }
            p += desc_len;
        }

        if (in_ep_ != 0 && out_ep_ != 0) {
            err = usb_host_interface_claim(client_handle_, device_handle_, iface_num_, 0);
            if (err != ESP_OK) {
                ESP_LOGE("UsbHostMidi", "Failed to claim interface");
                return;
            }

            // Allocate transfers: MPS is typically 64 for Full-Speed Bulk
            size_t in_buf_size = (in_mps_ > 0) ? in_mps_ : 64;
            usb_host_transfer_alloc(in_buf_size, 0, &transfer_in_);
            transfer_in_->device_handle = device_handle_;
            transfer_in_->bEndpointAddress = in_ep_;
            transfer_in_->callback = transferInCb;
            transfer_in_->context = this;
            transfer_in_->num_bytes = in_buf_size;

            usb_host_transfer_alloc(512, 0, &transfer_out_);
            transfer_out_->device_handle = device_handle_;
            transfer_out_->bEndpointAddress = out_ep_;
            transfer_out_->callback = transferOutCb;
            transfer_out_->context = this;
            transfer_out_busy_ = false;
            if (txMutex_) xSemaphoreTake(txMutex_, portMAX_DELAY);
            txQueue_.clear();
            if (txMutex_) xSemaphoreGive(txMutex_);

            isDeviceConnected_ = true;
            Serial.printf("[USB Host] Device connected! VID: 0x%04X, PID: 0x%04X\n", dev_desc->idVendor, dev_desc->idProduct);
            Serial.printf("[USB Host] Endpoints: IN=0x%02X (mps %u), OUT=0x%02X (mps %u)\n", in_ep_, in_mps_, out_ep_, out_mps_);

            // Submit initial IN transfer
            err = usb_host_transfer_submit(transfer_in_);
            if (err != ESP_OK) {
                ESP_LOGE("UsbHostMidi", "Failed to submit IN transfer: 0x%x", err);
                Serial.printf("[USB Host Error] Failed to submit initial IN transfer: 0x%x\n", err);
            } else {
                ESP_LOGI("UsbHostMidi", "MIDI interface ready and listening");
                Serial.println("[USB Host] MIDI interface claimed & actively listening for MPK Mini MK4 events!");
            }

            // Settle endpoint, release MK4 screen ownership to native firmware, and initialize preset & pre-seed all 8 hardware knobs!
            vTaskDelay(pdMS_TO_TICKS(50));
            controlSurface_.getDisplayDriver().releaseScreenOwnership();
            controlSurface_.selectPreset(controlSurface_.getCurrentPreset());
            // Read the MPK's current octave/transpose; later changes arrive as 0x6A notifications
            controlSurface_.requestKeybedState();
        }
    }

    void handleDeviceGone() {
        ESP_LOGI("UsbHostMidi", "Device disconnected");
        Serial.println("[USB Host] MPK Mini MK4 Disconnected!");
        isDeviceConnected_ = false;

        if (txMutex_) xSemaphoreTake(txMutex_, portMAX_DELAY);
        txQueue_.clear();
        transfer_out_busy_ = false;
        if (txMutex_) xSemaphoreGive(txMutex_);
        
        // Silence all sustaining voices immediately so notes never scream indefinitely on disconnect!
        if (synthEngine_) {
            synthEngine_->allNotesOff();
        }

        if (transfer_in_) {
            usb_host_transfer_free(transfer_in_);
            transfer_in_ = nullptr;
        }
        if (transfer_out_) {
            usb_host_transfer_free(transfer_out_);
            transfer_out_ = nullptr;
        }
        
        if (device_handle_) {
            usb_host_interface_release(client_handle_, device_handle_, iface_num_);
            usb_host_device_close(client_handle_, device_handle_);
            device_handle_ = nullptr;
        }
        
        in_ep_ = 0;
        out_ep_ = 0;
    }

    static void transferInCb(usb_transfer_t* transfer) {
        UsbHostMidi* self = static_cast<UsbHostMidi*>(transfer->context);
        if (!self) return;

        bool isCompleted = (transfer->status == USB_TRANSFER_STATUS_COMPLETED);
        uint8_t localBuf[64];
        int numBytes = 0;

        if (isCompleted) {
            numBytes = transfer->actual_num_bytes;
            if (numBytes > 64) numBytes = 64;
            if (numBytes > 0) {
                std::memcpy(localBuf, transfer->data_buffer, numBytes);
            }
        } else {
            Serial.printf("[USB Host Warn] IN transfer status: %d (not completed)\n", (int)transfer->status);
            if (transfer->status == USB_TRANSFER_STATUS_STALL && transfer->device_handle) {
                usb_host_endpoint_clear(transfer->device_handle, transfer->bEndpointAddress);
            }
        }

        // IMMEDIATELY re-submit transfer to hardware so USB host never has a blackout gap!
        if (self->isDeviceConnected_ && transfer->device_handle && transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
            transfer->num_bytes = (self->in_mps_ > 0) ? self->in_mps_ : 64;
            esp_err_t err = usb_host_transfer_submit(transfer);
            if (err != ESP_OK) {
                Serial.printf("[USB Host Error] usb_host_transfer_submit IN failed: 0x%x\n", err);
            }
        }

        // NOW process received MIDI packets safely outside the critical re-submission path
        if (isCompleted && numBytes >= 4) {
            for (int i = 0; i + 4 <= numBytes; i += 4) {
                uint8_t cable = (localBuf[i] >> 4) & 0x0F;
                uint8_t cin   = localBuf[i] & 0x0F;
                uint8_t b1    = localBuf[i+1];
                uint8_t b2    = localBuf[i+2];
                uint8_t b3    = localBuf[i+3];

                if (cin >= 0x4 && cin <= 0x7) {
                    self->feedUsbSysExPacket(cin, b1, b2, b3);
                } else if (cin >= 0x8 && cin <= 0xF) {
                    // Filter duplicate NoteOn/NoteOff from secondary virtual cables (Cable > 0)
                    // The MPK Mini MK4 mirrors NoteOn/NoteOff on Cable 2 (Plugin Port). Cable 0 (MIDI Port) is the primary performance port.
                    if ((cin == 0x8 || cin == 0x9) && cable > 0) {
                        continue;
                    }
                    // DAW Port (cable 1) transport buttons CC 73/74/76/77/78 are not synth parameters (RE doc §6.2)
                    if (cin == 0xB && cable == 1 &&
                        (b2 == 73 || b2 == 74 || (b2 >= 76 && b2 <= 78))) {
                        continue;
                    }
                    // Filter duplicate physical knob turns (CC 24..31) from secondary virtual cables (Cable > 0)
                    if (cin == 0xB && b2 >= Synth::CC_HW_KNOB_1 && b2 <= Synth::CC_HW_KNOB_8 && cable > 0) {
                        continue;
                    }
                    if (b1 < 0xF8) {
                        MIDI_LOG("[USB Cab%u] CIN 0x%X: %02X %02X %02X\n", cable, cin, b1, b2, b3);
                    }
                    self->processMidiMessage(b1, b2, b3);
                }
            }
        }
    }

    static void transferOutCb(usb_transfer_t* transfer) {
        UsbHostMidi* self = static_cast<UsbHostMidi*>(transfer->context);
        if (!self) return;

        if (transfer->status == USB_TRANSFER_STATUS_STALL && transfer->device_handle) {
            usb_host_endpoint_clear(transfer->device_handle, transfer->bEndpointAddress);
        }

        if (self->txMutex_) xSemaphoreTake(self->txMutex_, portMAX_DELAY);
        self->transfer_out_busy_ = false;
        self->pumpTxQueueLocked();
        if (self->txMutex_) xSemaphoreGive(self->txMutex_);
    }
#endif
};

} // namespace HardwareDriver
