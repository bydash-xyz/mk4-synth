#pragma once

#include <cstdint>

struct MockTwoWire {
    int sdaPin{-1};
    int sclPin{-1};
    uint32_t frequency{0};
    bool initialized{false};

    bool begin(int sda = -1, int scl = -1, uint32_t freq = 100000) {
        sdaPin = sda;
        sclPin = scl;
        frequency = freq;
        initialized = true;
        return true;
    }
};

inline MockTwoWire Wire;
