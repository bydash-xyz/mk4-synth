"""PlatformIO pre-build script: make BLE-MIDI 2.2 compile against arduino-esp32 3.x.

The library's BLEMIDI_ESP32.h reads the characteristic value into a std::string, but
BLECharacteristic::getValue() returns an Arduino String there. The libdeps folder is git-ignored and
recreated per environment, so patch it here instead of by hand.
"""
import os

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

header = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),  # noqa: F821
                      "BLE-MIDI", "src", "hardware", "BLEMIDI_ESP32.h")
old = "std::string rxValue = characteristic->getValue();"
new = "String rxValue = characteristic->getValue();"

if os.path.isfile(header):
    with open(header) as f:
        text = f.read()
    if old in text:
        with open(header, "w") as f:
            f.write(text.replace(old, new))
        print("patch_ble_midi: patched", header)
