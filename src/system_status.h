#pragma once

#include <cstdint>
#include <mutex>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#elif defined(MOCK_HAL)
#include "Arduino.h"
#endif

namespace HardwareDriver {

// =========================================================================
// System faults shown on the SSD1306 (serial is usually not attached).
//
// Enum order is display priority: the first set fatal fault owns the full-screen fault page, the first set
// warning owns the header tag. Fault state is synchronized with a mutex so watchers, setup,
// and the OLED task can safely update and inspect faults without race conditions.
// =========================================================================
enum class SystemFault : uint8_t {
    AudioDac = 0,        // fatal: I2S channel for the PCM5102A could not be created / enabled
    AudioTask,           // fatal: audio task could not be created (out of internal RAM)
    MpkMissing,          // warning: MPK Mini MK4 not enumerated on USB Host (boot timeout or unplugged)
    PluginPortOffline,   // warning: MK4 enumerated but no Akai SysEx reply (knob pre-seeding / display offline)
    Nvs,                 // warning: NVS could not be opened, user patches are not saved / restored
    Ble,                 // warning: BLE controller did not start, no wireless SysEx patch updates
    Psram,               // warning: PSRAM not detected (the synth itself only uses internal RAM)
    Count
};

constexpr uint16_t faultBit(SystemFault f) { return static_cast<uint16_t>(1u << static_cast<uint8_t>(f)); }

struct FaultInfo {
    bool        fatal;
    const char* title;   // fatal page title (6x10, <= 21 chars)
    const char* hint1;   // fatal page hint lines (6x10, <= 21 chars)
    const char* hint2;
    const char* tag;     // header tag for warnings (5x7, <= 8 chars)
    const char* banner;  // transient banner for warnings (5x7, <= 25 chars)
    const char* log;     // serial line
};

class SystemStatus {
public:
    static constexpr uint32_t MPK_ENUM_TIMEOUT_MS    = 5000; // no MK4 on USB after boot -> MpkMissing
    static constexpr uint32_t PLUGIN_PORT_TIMEOUT_MS = 3000; // ~10 missed keybed-state polls (300 ms each)
    static constexpr uint32_t BANNER_DURATION_MS     = 3000;

    static const FaultInfo& info(SystemFault f) {
        static constexpr FaultInfo TABLE[static_cast<int>(SystemFault::Count)] = {
            { true,  "NO AUDIO", "I2S DAC init failed", "check PCM5102A wiring", "",         "",
              "I2S / PCM5102A DAC init failed - no audio output" },
            { true,  "NO AUDIO", "audio task failed",   "low memory - reboot",  "",         "",
              "audio task could not start (out of memory) - no audio output" },
            { false, "",         "",                    "",                     "NO MPK",   "MPK Mini MK4 unplugged",
              "MPK Mini MK4 not detected on USB Host" },
            { false, "",         "",                    "",                     "PORT OFF", "MPK Plugin Port offline",
              "MPK Plugin Port not responding - knob seeding and MK4 screen offline" },
            { false, "",         "",                    "",                     "NO NVS",   "Patches won't be saved",
              "NVS init failed - user patches will not be saved or restored" },
            { false, "",         "",                    "",                     "NO BLE",   "BLE patch link off",
              "BLE init failed - wireless SysEx patch updates unavailable" },
            { false, "",         "",                    "",                     "NO PSRAM", "PSRAM not detected",
              "PSRAM not detected (N16R8 board expects 8 MB)" },
        };
        return TABLE[static_cast<int>(f)];
    }

    static constexpr uint16_t FATAL_MASK = faultBit(SystemFault::AudioDac) | faultBit(SystemFault::AudioTask);

    // Raise a fault; logs to Serial and starts the warning banner only on the clear -> set transition
    void set(SystemFault f, uint32_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        setLocked(f, nowMs);
    }

    void clear(SystemFault f) {
        std::lock_guard<std::mutex> lock(mutex_);
        clearLocked(f);
    }

    bool has(SystemFault f) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (mask_ & faultBit(f)) != 0;
    }
    uint16_t mask() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mask_;
    }
    bool hasFatal() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (mask_ & FATAL_MASK) != 0;
    }
    uint16_t fatalMask() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mask_ & FATAL_MASK;
    }
    uint16_t warningMask() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mask_ & static_cast<uint16_t>(~FATAL_MASK);
    }

    // Highest-priority set fault among `m` (m must be non-zero)
    static SystemFault first(uint16_t m) {
        uint8_t i = 0;
        while (i < static_cast<uint8_t>(SystemFault::Count) && !(m & (1u << i))) ++i;
        return static_cast<SystemFault>(i);
    }
    static uint8_t countBits(uint16_t m) {
        uint8_t n = 0;
        for (; m; m &= static_cast<uint16_t>(m - 1)) ++n;
        return n;
    }

    // Warning banner still on screen? (shown for BANNER_DURATION_MS after a warning is raised)
    bool bannerFault(uint32_t nowMs, SystemFault& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!bannerActive_) return false;
        if (nowMs - bannerSinceMs_ >= BANNER_DURATION_MS) {
            bannerActive_ = false;
            return false;
        }
        out = bannerFault_;
        return true;
    }

    // MK4 not seen on USB since boot: the OLED shows the "Connect MPK Mini MK4" page instead of the idle UI
    bool isWaitingForMpk() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return linkStarted_ && !mpkEverConnected_;
    }

    // MK4 link watcher, called periodically. `mpkSysExCount` counts inbound Akai SysEx (keybed-state replies to
    // the 300 ms poll, heartbeat pings); if it stops moving while enumerated the Plugin Port is offline.
    void updateMpkLink(uint32_t nowMs, bool usbConnected, uint32_t mpkSysExCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!linkStarted_) {
            linkStarted_ = true;
            bootMs_ = nowMs;
        }
        if (usbConnected) {
            if (!mpkConnected_) {
                mpkConnected_ = true;
                mpkEverConnected_ = true;
                lastReplyMs_ = nowMs;
                lastSysExCount_ = mpkSysExCount;
                clearLocked(SystemFault::MpkMissing);
            }
            if (mpkSysExCount != lastSysExCount_) {
                lastSysExCount_ = mpkSysExCount;
                lastReplyMs_ = nowMs;
            }
            if (nowMs - lastReplyMs_ >= PLUGIN_PORT_TIMEOUT_MS) {
                setLocked(SystemFault::PluginPortOffline, nowMs);
            } else {
                clearLocked(SystemFault::PluginPortOffline);
            }
        } else {
            mpkConnected_ = false;
            clearLocked(SystemFault::PluginPortOffline);
            if (mpkEverConnected_ || nowMs - bootMs_ >= MPK_ENUM_TIMEOUT_MS) {
                setLocked(SystemFault::MpkMissing, nowMs);
            }
        }
    }

    // Number of Serial fault lines emitted (host tests check each transition logs once)
    uint32_t getLogCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return logCount_;
    }

private:
    void setLocked(SystemFault f, uint32_t nowMs) {
        const uint16_t b = faultBit(f);
        if (mask_ & b) return;
        mask_ |= b;
        ++logCount_;
        if (!info(f).fatal) {
            bannerFault_ = f;
            bannerSinceMs_ = nowMs;
            bannerActive_ = true;
        }
#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
        Serial.printf("[Fault] %s\n", info(f).log);
#endif
    }

    void clearLocked(SystemFault f) {
        const uint16_t b = faultBit(f);
        if (!(mask_ & b)) return;
        mask_ &= static_cast<uint16_t>(~b);
        ++logCount_;
        if (bannerActive_ && bannerFault_ == f) bannerActive_ = false;
#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)
        Serial.printf("[Fault cleared] %s\n", info(f).log);
#endif
    }

    mutable std::mutex mutex_;
    uint16_t mask_{0};
    uint32_t logCount_{0};

    bool bannerActive_{false};
    SystemFault bannerFault_{SystemFault::Count};
    uint32_t bannerSinceMs_{0};

    bool linkStarted_{false};
    bool mpkConnected_{false};
    bool mpkEverConnected_{false};
    uint32_t bootMs_{0};
    uint32_t lastReplyMs_{0};
    uint32_t lastSysExCount_{0};
};

} // namespace HardwareDriver
