# Abstract MPK Mini MK4 Control Surface with SysEx 0x12 Hardware Knob Pre-Seeding

## Context
Interoperability testing of the Akai MPK Mini IV showed that the 8 endless rotary encoders (`K1..K8`) run in `Mode 0 (Absolute)` backed by internal per-knob accumulators. At boot or patch change, unseeded accumulators hold `0` (or stale values). When turned, the firmware increments or decrements from `0`, causing synth parameters to drop immediately to 0.

Standard MIDI CC messages (`0xB0`) sent to the device don't update those accumulators, and 284-byte RAM preset writes (`0x64`) contain no initial value field. SysEx Command `0x12` (`F0 47 00 5D 12 00 02 <k> <val> F7`) sets a knob's accumulator directly.

Previously in `mk4-synth`, MPK Mini MK4 input handling was tightly coupled inside `HardwareDriver::UsbHostMidi`, while display operations were split between `MpkSysExDisplay` and `SynthEngine::MpkMiniMk4::DisplayDriver`.

## Decision
We create a dedicated, platform-agnostic deep module **`MpkMiniMk4::ControlSurface`** in `lib/MpkMiniMk4Display`:
1. **Knob Value Pre-Seeding (`SysEx 0x12`)**: On preset load, Bank A/B switch, and external parameter changes (e.g. Web Companion SysEx), the control surface transmits 10-byte `0x12` packets over `Plugin Port` to seed each knob's accumulator in the MPK Mini IV.
2. **Echo Suppression (`ChangeSource != PhysicalKnob`)**: When a parameter change originates from turning physical knob `K1..K8`, echoing `SysEx 0x12` back to the hardware encoder is suppressed to prevent bus saturation and race conditions with ongoing quadrature encoder clicks.
3. **Dual-Bank Live Session Cache**: Live 7-bit values for Bank A (`K1..K8`) and Bank B (`K9..K16`) are maintained per preset session, ensuring tweaks made in Bank A are not lost when editing Bank B and vice versa.
4. **Mandatory Plugin Port Connection**: An active `Plugin Port` SysEx connection is strictly required for parameter control. If `Plugin Port` is not connected, the control surface halts parameter updates and alerts the user to connect `MPK mini IV Plugin Port`.
5. **Screen Owner Heartbeat Keepalive Protocol (`SysEx 0x19`)**: Reverse engineering confirmed that the MPK Mini IV hardware controller emits a Heartbeat Ping (`F0 47 <dev> 5D 19 00 00 F7`) every 2.0 seconds over `Plugin Port`. If the host fails to acknowledge with a Keepalive Pong (`F0 47 <dev> 5D 19 00 11 <owner> <uuid16> F7`) within 3 cycles (6.0 seconds), the controller automatically revokes screen ownership and reverts to the default `Prst2: PLUGIN` screen, ignoring subsequent display updates. `DisplayDriver` and `ControlSurface` now implement both reactive ping handling and proactive 1.5s keepalive broadcasts.
6. **Adapter Seam Preservation**: `HardwareDriver::UsbHostMidi` wraps `MpkMiniMk4::ControlSurface` for ESP32-S3 USB Host, keeping existing interfaces for `main.cpp`, `main_sim.cpp`, and WASM intact.

## Consequences
- Eliminates knob value drop-to-zero bugs when starting or switching synth presets.
- Seamlessly preserves parameter states across Bank A (`SHIFT` released) and Bank B (`SHIFT` held).
- Prevents the MPK Mini IV OLED from reverting back to `Prst2: PLUGIN` after 6 seconds of idle time.
- Ensures exact parameter numbers display on the MPK Mini IV's built-in OLED popup.
- Hardware transport (`UsbHostMidi`) is decoupled from MPK control surface logic.
