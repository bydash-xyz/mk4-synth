#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <thread>
#include <map>

// Standard Arduino type definitions
using boolean = bool;
using byte = uint8_t;

constexpr uint8_t A0 = 14;
constexpr uint8_t INPUT = 0;
constexpr uint8_t OUTPUT = 1;
constexpr uint8_t INPUT_PULLUP = 2;
constexpr uint8_t HIGH = 1;
constexpr uint8_t LOW = 0;
constexpr uint32_t SERIAL_8N1 = 0x06;

// Mock FreeRTOS definitions
#ifndef FREERTOS_MOCKS_DEFINED
#define FREERTOS_MOCKS_DEFINED

#ifndef configMAX_PRIORITIES
#define configMAX_PRIORITIES 25
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xFFFFFFFFUL
#endif

#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);
using BaseType_t = long;
using TickType_t = uint32_t;

inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
inline void vTaskDelay(TickType_t ticks) {}

struct MockFreeRTOS {
    static inline TaskFunction_t lastTaskFunction = nullptr;
    static inline void* lastTaskParam = nullptr;
    static inline const char* lastTaskName = nullptr;
    static inline int lastPinnedCore = -1;
    static inline uint32_t lastPriority = 0;

    static void reset() {
        lastTaskFunction = nullptr;
        lastTaskParam = nullptr;
        lastTaskName = nullptr;
        lastPinnedCore = -1;
        lastPriority = 0;
    }
};

inline BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t pvTaskCode,
    const char* const pcName,
    const uint32_t usStackDepth,
    void* const pvParameters,
    uint32_t uxPriority,
    TaskHandle_t* const pvCreatedTask,
    const BaseType_t xCoreID
) {
    MockFreeRTOS::lastTaskFunction = pvTaskCode;
    MockFreeRTOS::lastTaskParam = pvParameters;
    MockFreeRTOS::lastTaskName = pcName;
    MockFreeRTOS::lastPriority = uxPriority;
    MockFreeRTOS::lastPinnedCore = static_cast<int>(xCoreID);
    if (pvCreatedTask) {
        *pvCreatedTask = reinterpret_cast<void*>(0x1234);
    }
    return pdPASS;
}

#endif // FREERTOS_MOCKS_DEFINED

// Mock Serial implementation for unit tests
class MockSerial {
public:
    void begin(uint32_t baud, uint32_t config = SERIAL_8N1, int rx = -1, int tx = -1) {
        baud_ = baud;
        isOpen_ = true;
    }

    int available() const {
        return static_cast<int>(rxBuffer_.size());
    }

    int read() {
        if (rxBuffer_.empty()) return -1;
        uint8_t byte = rxBuffer_.front();
        rxBuffer_.pop_front();
        return byte;
    }

    size_t write(uint8_t b) {
        txBuffer_.push_back(b);
        return 1;
    }

    void print(const char* s) {}
    void print(float f) {}
    void print(int i) {}
    void println(const char* s) {}
    void println(float f) {}
    void println(int i) {}
    void println() {}

    void printf(const char* fmt, ...) {}

    // Test instrumentation helpers
    void injectByte(uint8_t byte) {
        rxBuffer_.push_back(byte);
    }

    void injectBytes(const std::vector<uint8_t>& bytes) {
        for (uint8_t b : bytes) {
            rxBuffer_.push_back(b);
        }
    }

    void clear() {
        rxBuffer_.clear();
        txBuffer_.clear();
    }

    bool isOpen() const { return isOpen_; }
    uint32_t getBaud() const { return baud_; }
    const std::vector<uint8_t>& getTxBuffer() const { return txBuffer_; }

private:
    bool isOpen_{false};
    uint32_t baud_{0};
    std::deque<uint8_t> rxBuffer_;
    std::vector<uint8_t> txBuffer_;
};

// Global serial instances declared in Arduino core
inline MockSerial Serial;
inline MockSerial Serial1;
inline MockSerial Serial2;

// Mock DAC & Pin state
inline uint8_t g_analogWriteResolution = 8;
inline uint16_t g_analogWriteLastA0 = 0;

inline void analogWriteResolution(uint8_t bits) {
    g_analogWriteResolution = bits;
}

inline void analogWrite(uint8_t pin, uint16_t val) {
    if (pin == A0) {
        g_analogWriteLastA0 = val;
    }
}

inline void delay(uint32_t ms) {}
inline void delayMicroseconds(uint32_t us) {}

inline uint32_t g_mockMillisOverride = 0;
inline bool g_useMockMillisOverride = false;

inline void setMockMillis(uint32_t ms) {
    g_mockMillisOverride = ms;
    g_useMockMillisOverride = true;
}

inline uint32_t millis() {
    if (g_useMockMillisOverride) return g_mockMillisOverride;
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

// Pins millis()/micros() at their current value for the guard's lifetime, so timing-sensitive input (e.g. the
// BANK A/B 400 ms hold) can't be skewed by a slow or descheduled test process. Time never runs backwards.
struct ScopedFrozenMillis {
    bool wasOverridden = g_useMockMillisOverride;
    uint32_t previous = g_mockMillisOverride;
    ScopedFrozenMillis() { setMockMillis(millis()); }
    ~ScopedFrozenMillis() {
        g_mockMillisOverride = previous;
        g_useMockMillisOverride = wasOverridden;
    }
    ScopedFrozenMillis(const ScopedFrozenMillis&) = delete;
    ScopedFrozenMillis& operator=(const ScopedFrozenMillis&) = delete;
};

inline uint32_t micros() {
    if (g_useMockMillisOverride) return g_mockMillisOverride * 1000U;
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
}

inline void pinMode(uint8_t pin, uint8_t mode) {}
inline int digitalRead(uint8_t pin) { return HIGH; }
inline void digitalWrite(uint8_t pin, uint8_t val) {}
inline uint16_t analogRead(uint8_t pin) { return 2048; }
inline void noInterrupts() {}
inline void interrupts() {}
