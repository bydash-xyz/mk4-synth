# Dual-Rate SysEx Patch Synchronization and Front-Loaded Factory / User Bank Split

We adopt a dual-rate MIDI System Exclusive (SysEx) protocol for hardware patch synchronization and partition the 32-slot preset bank with immutable Factory ROM presets packed at the front (`Slots #1..FACTORY_PRESET_COUNT`) and all remaining capacity reserved as non-volatile User Customizable Slots (`Slots #FACTORY_PRESET_COUNT+1..#32`).

> **Partly superseded by [ADR-0006](./0006-parameter-list-patch-format.md):** the 48-byte payload packed into a 62-byte dump is replaced by the `PatchDumpV2` (`0x05`) parameter list. The 30 Hz fast parameter stream, the factory / user slot split and the ACK/NACK handshake still stand.

## Context
The companion app and hardware controller must stream live parameter adjustments to connected microcontrollers (ESP32-S3, ESP32, and Arduino UNO R4) over USB/BLE/DIN MIDI with zero perceptible latency, while allowing new factory instrument models to be added over time and reserving the rest of the 32-slot address space for persistent user patches.

## Considered Options
1. **Dual-Rate SysEx + Contiguous Front Factory / Trailing User Partition**: Fast parameter edits stream at 30 Hz using a lightweight 10-byte message (`F0 7D 01 01 <ParamID> <ValMSB> <ValLSB> <Slot> <Reserved> F7`) with no handshake directly to active RAM or a target User Slot. Full patch transfers and saves use a 48-byte payload packed into 55 7-bit MIDI bytes (62 bytes total), verified with an XOR checksum and confirmed with an MCU ACK/NACK handshake. All Factory ROM models (Subtractive Analog `#1..#6`, Casio PT-10 `#7..#10`, 2-Op FM `#11..#15`) sit contiguously at the front (`1..FACTORY_PRESET_COUNT`), and all remaining slots up to `TOTAL_SLOT_COUNT` (`#16..#32`, `User_01..User_17`) are NVS-backed user customizable slots.
2. **Hardcoding a Single Fixed User Slot in the Middle of the Bank (Legacy Slot #7)**: Pinning a single user slot at `#7` forced new factory models (Casio PT-10, FM keyboards) either to overwrite the user slot or fragment factory presets into non-contiguous ranges.
3. **Full Patch Dumps on Every Parameter Change**: Transmitting the entire 48-byte state on every mouse drag or knob twist saturates DIN/BLE MIDI links (~15ms per 48-byte message at 31,250 baud) and causes NVS flash wear.

## Consequences
- Live knob sweeps are fluid and glitch-free at 30 Hz in volatile RAM without flash wear on microcontrollers.
- Factory ROM presets (`#1..FACTORY_PRESET_COUNT`) remain contiguous and read-only; adding new factory models simply advances `USER_SLOT_START` (`FACTORY_PRESET_COUNT + 1`).
- Every remaining slot (`USER_SLOT_START..TOTAL_SLOT_COUNT`, currently Slots `#16..#32`) is independently writable and persisted in ESP32 NVS (`u_slot_16..u_slot_32`) after checksum validation.
