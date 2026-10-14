# Hardware Verification & Diagnostic Suite — Revision 2.0

This directory contains standalone, isolated hardware verification sketches for bringing up the **ESP32-S3 Rev 2.0 Synthesizer** incrementally.

Don't flash the full firmware first. Test each part with these sketches so a wiring fault shows up on its own. The parts, wiring and solder pads are in the build guide, [HARDWARE.md](../HARDWARE.md).

---

## Bring-Up Order & Pass/Fail Criteria

| Test | Sketch Directory | Hardware Module | Verification Objective | Pass Criteria |
| :--- | :--- | :--- | :--- | :--- |
| **01** | `01_system_health/` | **ESP32-S3-DevKitC-1** | 240MHz Clock, 16MB Flash, 8MB Octal PSRAM | Serial logs show 16MB Flash detected, 8MB PSRAM active, and 1MB memory write/read integrity check passes. |
| **02** | `02_i2s_dac_test/` | **PCM5102A I2S DAC** | GPIO 4 (BCK), GPIO 5 (LRCK), GPIO 6 (DIN) | Clear, distortion-free 440 Hz sine tone in headphones. Left-only, Right-only, and Stereo test modes verify channel wiring. |
| **03** | `03_i2c_oled_test/` | **0.96" SSD1306 OLED** | GPIO 8 (SDA), GPIO 9 (SCL) via 400kHz I2C | I2C scan detects device at `0x3C`. Bouncing ball graphic renders smoothly at ~40 FPS without screen tears. |
| **05** | `05_usb_mpk_test/` | **Akai MPK Mini MK4** | Native USB OTG (GPIO 19 D-, GPIO 20 D+) | MPK powers on via USB VBUS. Serial logs enumerate device and 284-byte RAM SysEx packet updates the MPK's OLED screen. |
| **06** | `06_ble_midi_nvs_test/` | **BLE MIDI & NVS Flash** | Bluetooth LE + Onboard Flash | Device advertises as `"MK4-Synth"`. SysEx patch drop writes the patch to NVS and persists across reboot. |

---

## Wiring

See the wiring diagram and pin tables in [HARDWARE.md](../HARDWARE.md#4-wire-it).

---

## How to Flash a Diagnostic Sketch

Using the Arduino IDE or PlatformIO CLI:

The computer always connects to the ESP32-S3 **COM/UART** port; the native **USB** port is reserved for the MPK Mini MK4. Build every sketch with `Serial` on UART0 (`-DARDUINO_USB_MODE=0 -DARDUINO_USB_CDC_ON_BOOT=0`, unflagging the board's `=1` defaults). With the board's default USB CDC serial, the sketch prints to the native USB port and looks dead: only the ROM boot log appears on COM.

```bash
# Diagnostic 02 has its own env (already UART serial)
pio run -e diag_dac_test -t upload
# Read the log (pio device monitor needs an interactive terminal)
~/.local/pipx/venvs/platformio/bin/python tools/serial_capture.py 15
```
