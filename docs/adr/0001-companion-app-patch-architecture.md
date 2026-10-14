# Companion App Architecture and Multi-Engine Patch Schema

> **Status: the browser companion app was retired on 2026-09-27 and is not part of this repository.** Patches are sent with [`tools/patch_sender.html`](../../tools/patch_sender.html). The `Preset` schema below is still what the firmware uses.

We build the synth companion designer as a browser-based Single Page App (Web Audio + Web MIDI) using a tagged multi-engine JSON patch schema (`Preset` / `SynthParameters`) and compact 48-byte union-multiplexed MIDI System Exclusive (`RawPatchData`) frames, avoiding both a runtime bytecode DSL and a native desktop GUI.

> **Partly superseded by [ADR-0006](./0006-parameter-list-patch-format.md):** the 48-byte union-multiplexed `RawPatchData` wire payload is replaced by the `PatchDumpV2` parameter list, where every value has its own ID. The browser companion architecture and the tagged multi-engine `Preset` schema still stand.

## Context
We need a visual patch designer and hardware librarian that simulates multiple distinct hardware synthesizer families—**Classic Subtractive Analog** (Juno-60, Minimoog, Prophet-5, TB-303, CS-80, OB-Xa), **Casio PT-10 Lo-Fi Digital Pulse/Harmonic**, and **2-Operator Polyphonic FM Keyboards** (DX7 E.Piano, Rhodes, Clavinet, Organ)—while keeping the over-the-wire SysEx patch dump compact enough (48 bytes raw / 62 bytes 7-bit MIDI) to transfer reliably over USB, BLE, and 31.25 kbaud DIN MIDI.

## Considered Options
1. **Web SPA + Tagged Multi-Engine `Preset` Struct + Union-Multiplexed 48-Byte Wire Payload**: Zero-install browser app with Web Audio preview and Web MIDI SysEx sync. In C++ and JSON, `Preset` (`SynthParameters`) holds the superset of Subtractive, Casio PT-10, and FM parameters discriminated by `EngineMode` (`Subtractive` vs `FM`) and `SynthModel`. On the wire (`RawPatchData`), engine-exclusive fields share fixed 16-bit slots keyed by `engineMode` (e.g. Subtractive `cutoffHz`, `resonanceQ`, `subOscLevel`, `filterEnvAmount` $\leftrightarrow$ FM `modDepth`, `modRatio`, `modFeedback`, `tremoloDepth`) so all device models fit inside a single 62-byte SysEx packet.
2. **Separate Wire Packet Formats Per Emulated Device**: Defining distinct SysEx frame sizes and decoders for Subtractive, Casio PT-10, and FM engines. Rejected because variable-length patch dumps complicate NVS storage slots, BLE MTU packet fragmentation, and ACK/NACK verification.
3. **Custom Textual DSL + Bytecode VM / Native Desktop GUI (JUCE / Dear ImGui)**: Rejected due to MCU RAM/flash constraints (32 KB SRAM on UNO R4) and cross-platform desktop installer friction.

## Consequences
- **ADR-0001 remains active**: It governs the Web SPA + Web MIDI/Audio companion architecture and explains why `RawPatchData` intentionally multiplexes Subtractive and FM parameters across a fixed 48-byte binary frame rather than growing the SysEx payload per simulated device.
- Adding a new simulated synthesis engine extends `Preset` in C++/JSON while reusing the 48-byte `RawPatchData` envelope keyed by `engineMode`.
