# Use Native Akai MPK Mini IV `Slot 2 (PLUGIN)` Without `Slot 0` SysEx Overwrites and Unify MIDI Routing in C++ / WebAssembly

We use the Akai MPK Mini IV (`Model ID = 0x5D`)'s built-in factory **`Slot 2 (PLUGIN)`** preset (`K1..K8 = CC 24..31`, `mode = 0` Absolute `0..127`) directly without overwriting `Slot 0` (active hardware RAM) via SysEx `0x64`, and route all MIDI messages in both the Web Companion App and ESP32-S3 firmware through the single C++ `HardwareDriver::UsbHostMidi` driver compiled to WebAssembly (`window.esp32Wasm`).

> **Status:** the Web Companion App and its WebAssembly build were retired on 2026-09-27. The Slot 2 (PLUGIN) decision stands; `UsbHostMidi` is now shared by the firmware and the desktop simulator.

## Context
The Akai MPK Mini IV exposes 4 USB-MIDI ports (`MIDI Port`, `DAW Port`, `Plugin Port`, and `Software Control Port`) and 8 internal preset slots (`Slot 1 DAW`, `Slot 2 PLUGIN`, `Slots 3..8 USER1..6`) loaded into `Slot 0` (volatile active RAM). Previously, `syncMpkMiniKnobNames()` sent the 284-byte SysEx `0x64` RAM-write command to `Slot 0` to rename knob labels and remap `K1..K8` to `CC 70..77`. This corrupted the active hardware buffer, collided with preset navigation CCs (`CC 71`, `CC 72`, `CC 74`), and caused the physical knob mappings to break after screen syncs or preset changes. Additionally, maintaining a parallel JavaScript `midiCcMap` in `app.js` caused behavioral drift between the browser companion app and the ESP32-S3 firmware.

## Considered Options
1. **Native Factory `Slot 2 (PLUGIN)` (No `Slot 0` `0x64` Overwrites) + Unified C++/WASM `UsbHostMidi` Driver (Chosen)**:
   - Leave the MPK Mini IV's `Slot 0` RAM untouched on factory **`Slot 2 (PLUGIN)`**, which natively maps `K1..K4` (top row) to `CC 24..27` and `K5..K8` (bottom row) to `CC 28..31` in **Absolute mode (`mode = 0`, `0..127`)**, the 25-key keybed to Channel 1 (`Notes 48..72`, `C3..C5`), and the 8 MPC pads to Channel 10 (`Notes 36..51`).
   - If OLED status text is written to the MPK Mini IV (`writeMpkMiniDisplay`), use only non-destructive display-line SysEx commands (`0x68`, `0x6A`, `0x6B`) rather than overwriting `Slot 0` preset RAM (`0x64`).
   - Route all incoming MIDI CCs, Notes, Pitch Bend, and `SHIFT` (`CC 18`/`102`) in `app.js` directly through `window.esp32Wasm.processMidiMessage()` (`src/usb_host_midi.h`), while also accumulating relative 2's-complement deltas (`1..15` CW / `127..113` CCW) in `sysex.js` if `CC 24..31` arrives from `MPK mini IV DAW Port` (`Slot 1 DAW`).
2. **Overwriting `Slot 0` RAM via SysEx `0x64` (`syncMpkMiniKnobNames`)**:
   - Rejected because mutating `Slot 0` overwrites the controller's active preset state in volatile RAM, risks remapping CCs away from `CC 24..31`, and requires SysEx write permissions before knobs work properly.
3. **Using `Slot 1 (DAW)` as the Primary Preset**:
   - Rejected as the primary preset because `Slot 1 (DAW)` configures `K1..K8` (`CC 24..31`) with `mode = 1` (Relative 2's-complement endless encoder deltas `1` / `127` on `MPK mini IV DAW Port`), whereas `Slot 2 (PLUGIN)` provides native `0..127` absolute sweeps (`mode = 0`).
4. **Duplicate JavaScript MIDI CC Mapping Tables in `app.js`**:
   - Rejected because duplicate JS mapping logic diverges from the embedded C++ `UsbHostMidi` firmware and complicates debugging.

## Consequences
- **Zero Hardware State Corruption**: The MPK Mini IV remains permanently on its unmodified factory `Slot 2 (PLUGIN)` configuration (`CC 24..31`, `mode = 0`), eliminating any possibility of SysEx screen updates breaking knob CCs.
- **Single Source of Truth (`src/usb_host_midi.h`)**: Both the ESP32-S3 hardware synth and the HTML5 Companion App execute the exact same compiled C++ `UsbHostMidi::processMidiMessage` logic for `K1..K8` (`CC 24..31`), `SHIFT` Bank B gating (`K9..K16`), preset stepping (`CC 71`/`74`/`72`/`14`/`16`/`17`), and `Mod Wheel` (`CC 1`).
- **Full Reference Documentation**: The ground-truth 284-byte RAM layout and port mapping extracted from the physical hardware are documented in [`docs/MPK_MINI_IV_MIDI_REFERENCE.md`](../MPK_MINI_IV_MIDI_REFERENCE.md).
