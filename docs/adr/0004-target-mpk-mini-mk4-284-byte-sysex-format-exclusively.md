# Target MPK Mini MK4 (284-byte) SysEx Format Exclusively and Forbid Legacy MK3 (183-byte) Formats

## Context
When dynamically pushing patch names and knob label updates to the Akai MPK Mini's OLED screen via a SysEx RAM dump (`CMD_RAM_WRITE_HI = 0x64`), the underlying memory structure of the hardware must be respected. 

Previously, the C++ `MpkSysExDisplay::buildRamDump` implementation utilized the legacy MPK Mini MK3 SysEx template, which expects a **183-byte payload** (`Model ID 0x49`, `CMD 0x67 0x01`). In the MK3 format, the memory map places the 8 Knob Descriptors directly following the 16-byte Program Name (at byte offset `22`).

However, the MPK Mini MK4 uses a completely redesigned **284-byte payload** (`Model ID 0x5D`, `CMD 0x64 0x02`). In the MK4 memory map, the space following the Program Name is reserved for Encoder bindings, 16 MPC Pad configurations, and `-/+` Navigation button states. The Knob Descriptors do not begin until byte offset `117`.

When the ESP32-S3 driver dispatched the 183-byte MK3 payload to an MPK Mini MK4 device, the hardware partially accepted the write due to legacy compatibility quirks, but applied the Knob Descriptors starting at offset `22`. This inadvertently **overwrote and corrupted the memory assignments for the Encoder and -/+ navigation buttons**, causing them to stop emitting MIDI CCs entirely. Furthermore, because the hardware knobs were remapped to dynamic Target CCs (e.g. Cutoff 74) instead of their required static `CC 24..31` mappings, the C++ firmware's `UsbHostMidi::handleControlChange` parser could no longer intercept them. This resulted in catastrophic failure of the hardware control surface ("all 8 knobs, encoder, and -/+ buttons no longer work").

## Decision
We **exclusively target the MPK Mini MK4** and its exact 284-byte SysEx memory map across all environments (C++ Firmware, Web Companion App, and Python tooling). The legacy 183-byte MK3 payload is strictly forbidden.

When generating a RAM dump (`0x64`), the system must:
1. Emit exactly 284 bytes.
2. Use Model ID `0x5D` and Write Command `0x64 0x02`.
3. Preserve the exact padding and offsets for the MK4 (e.g., Program Name at offset `8`, Knob Descriptors starting at offset `117`).
4. Pin the 8 hardware Knob Descriptors to `CC 24..31` in Absolute Mode (`0..127`), allowing the C++ host to route them dynamically.
5. Pad ASCII strings (Program Name and Knob Labels) with `0x00` null terminators, not spaces.

## Consequences
- **Elimination of Hardware Corruption**: The MPK Mini MK4 will safely parse the full 284-byte payload, preserving the CC mappings of the Encoder and Pad elements.
- **Consistent Routing**: By strictly enforcing `CC 24..31` on the knobs, the `UsbHostMidi` parser can reliably intercept and map them to the active synth engine parameters.
- **Deprecation of MK3**: The system will not attempt to support the legacy MK3 hardware or its 183-byte format.
