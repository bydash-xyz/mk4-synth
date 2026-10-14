# Parameter-List Patch Format (`PatchDumpV2`)

**Status: Accepted.** Supersedes the union-multiplexed `RawPatchData` wire payload of ADR-0001 and the 48 / 55 / 62-byte patch dump in ADR-0002. The fast parameter stream, slot layout and ACK/NACK handshake of ADR-0002 are kept.

A full patch is sent and stored as a list of `(ParamId, 14-bit value)` entries. It uses the same parameter IDs and value encoding as the 30 Hz `FastParamUpdate` stream, under a new command `0x05 PatchDumpV2`. Each ID has exactly one meaning. IDs a receiver doesn't know are skipped, and IDs a patch leaves out take their defaults. The format therefore never needs a size decision, spare bytes, or a layout version.

## Context
ADR-0001 packed Subtractive and FM parameters into shared 16-bit fields keyed by `engineMode` so that every model fit in a 62-byte SysEx packet. That has cost:
- **Lost state.** `tremoloRateHz` has no field, so the decoder hardcodes 4.5 Hz. FM Warm Rhodes (4.2 Hz) and FM Drawbar Organ (6.2 Hz) change sound when cloned to a user slot. FM patches also decode with a fixed cutoff, resonance and filter-envelope amount.
- **Hidden meaning.** The same bytes mean cutoff or mod depth depending on bit 3 of `flags`, and bits 4..7 of `flags` hold the voicing. Every encoder (the firmware, `tools/patch_sender.html`, any companion app) has to reproduce that branching.
- **No room to grow.** The payload has no version and no spare space. Per-patch knob maps, an FM modulator envelope or LFO settings would each force a new layout.
- **Duplicated code.** `SynthEngine::handleSysEx` has two near-identical `ParamId` switches (one for user slots, one for the live sound), and `SysExProtocol` has a third encoding of the same ranges for dumps.

Firmware can be updated freely. The ESP32-S3 Rev 2.0 receives patches over BLE only. Its SysEx limit of 128 bytes is a MIDI Library default (`MySettings::SysExMaxSize`), not a BLE limit.

## Considered Options
1. **Parameter list with the `FastParamUpdate` vocabulary (chosen).** 98 bytes plus the name (at most 109) for a full patch today, and it grows only with what a patch actually contains.
2. **Larger fixed struct with dedicated fields and reserved bytes (96 bytes, 117-byte message).** Removes field reuse and fits the default 128-byte limit, but the spare room still runs out eventually, and it keeps the 7-bit packing and a separate dump encoding. Rejected.
3. **Keep 48 bytes and give tremolo rate a field FM doesn't use.** Deepens the multiplexing. Rejected.

## Wire Format

```
F0 7D 01 05 <slot> <nameLen> <name × nameLen> <entry>… <xor> F7
entry     = <id 0x00..0x7E> <msb> <lsb>                      (3 bytes)
          | 0x7F <idHi> <idLo> <msb> <lsb>                   (5 bytes, extended ID space)
```

- **`slot`:** 0 = live edit buffer (not saved), `USER_SLOT_START`..32 = user slot (24..32 since [ADR-0007](0007-additional-synthesis-engines.md); 16..32 before). Factory slots are rejected with `NackInvalidSlot`.
- **`nameLen`:** 0..11. **`name`:** ASCII `0x20..0x7E`.
- **Values:** 14-bit, `msb << 7 | lsb`. Every byte is already MIDI-safe, so no 7-in-8 packing is needed.
- **Checksum:** `xor` is the 7-bit XOR of every byte from `slot` to the last entry byte.
- **Entries are fixed-size**, so a receiver can always skip an entry it doesn't understand.
- **Unknown IDs:** ignored, including unknown extended IDs. **Duplicate IDs:** the last one wins.
- **Missing IDs:** take the defaults from `makeDefaultUserPreset` (`lib/SynthEngine/include/Presets.h`) and the `Preset` member defaults for FM fields.
- **Global IDs** (`MasterVolume 0x40`, metronome / key / scale `0x50..0x55`) are ignored inside a dump. They are performance state, not patch state.
- **Maximum message size:** 512 bytes. Larger messages are rejected with `NackCorrupted`.
- **Encoders send every patch-scope ID.** Omitting an ID is for forward compatibility, not for partial edits; those use `FastParamUpdate`.
- **Engine and voicing are sent resolved.** Factory presets carry them in their `SynthModel`, so a dump of slot 11 sends `EngineMode` = FM and `Voicing` = DX E-Piano + 1, as the engine applies them.
- **Replies.** A dump is answered with `F0 7D 01 0F 05 <status> F7` (`refCmd` = `0x05`): `AckSuccess` once applied (and, for a user slot, saved), otherwise `NackChecksumErr`, `NackInvalidSlot` or `NackCorrupted`.

### Patch-scope IDs

Continuous values: `value14 = round(norm × 16383)`. On `lin` curves `norm = (v − lo) / (hi − lo)`; on `log` curves `norm = ln(v / lo) / ln(hi / lo)`. Enums and booleans carry the raw index, matching how `FastParamUpdate` already decodes them: booleans and the on/off enums (`VoiceMode`, `PercHarmonic`) read any nonzero value as on, and the other enums (`Waveform`, `EngineMode`, `Voicing`, `WtTable`, `PdWave`) read values above their range as normalized. A sender that sends `EngineMode` = 1 for FM is unaffected by the engines ADR-0007 added.

| ID | Name | Encoding | Status |
|---|---|---|---|
| 0x01 | Cutoff | log 20..20000 Hz | existing |
| 0x02 | Resonance | lin 0.5..25 Q | existing |
| 0x03 | Waveform | enum 0 Saw, 1 Square, 2 Triangle | existing |
| 0x04 | SubOscLevel | lin 0..1 | existing |
| 0x05 | DetuneCents | lin 0..50 | existing |
| 0x06 | Drive | lin 1..3.5 | existing |
| 0x07 | FilterEnvAmount | lin 0..1 | existing |
| 0x08 | EngineMode | enum 0 Subtractive, 1 FM, 2 Wavetable, 3 Phase Distortion, 4 Pluck, 5 Additive (2..5: ADR-0007) | **new** |
| 0x09 | Voicing | enum factory `SynthModel` index + 1; 0 = generic user voicing | **new** |
| 0x10..0x13 | Amp A / D / S / R | log 1..4000 ms / log 5..5000 ms / lin 0..1 / log 5..5000 ms | existing |
| 0x14..0x17 | Filter A / D / S / R | as amp | existing |
| 0x20 | ChorusEnabled | bool | existing |
| 0x21 | ChorusRate | lin 0.1..5 Hz | existing |
| 0x22 | ChorusDepth | lin 0.5..5 ms | existing |
| 0x23 | ChorusMix | lin 0..1 | existing |
| 0x30 | VoiceMode | enum 0 Poly, 1 Mono | existing |
| 0x31 | GlideTime | lin 0..1000 ms | existing |
| 0x32 | PortamentoEnabled | bool | existing |
| 0x33 | LegatoEnabled | bool | **new** |
| 0x60 | ModRatio | lin 0.5..14 | **new** |
| 0x61 | ModDepth | lin 0..12 | **new** |
| 0x62 | ModFeedback | lin 0..1.5 | **new** |
| 0x63 | TremoloDepth | lin 0..1 | **new** |
| 0x64 | TremoloRate | lin 0.1..10 Hz | **new** |
| 0x65 | WtTable | enum 0 PPG Sweep, 1 Analog, 2 Vox, 3 Glass | ADR-0007 |
| 0x66 | WtPosition | lin 0..1 | ADR-0007 |
| 0x67 | WtEnvAmount | lin 0..1 | ADR-0007 |
| 0x68 | PdWave | enum 0 Saw, 1 Square, 2 Pulse, 3 Double Sine, 4..6 Resonance I..III | ADR-0007 |
| 0x69 | PdDcw | lin 0..1 | ADR-0007 |
| 0x6A | PluckDecay | log 100..10000 ms | ADR-0007 |
| 0x6B | PluckBright | lin 0..1 | ADR-0007 |
| 0x6C | PluckPick | lin 0..1 | ADR-0007 |
| 0x6D | PercLevel | lin 0..1 | ADR-0007 |
| 0x6E | PercHarmonic | enum 0 second, 1 third | ADR-0007 |
| 0x6F | KeyClick | lin 0..1 | ADR-0007 |
| 0x70..0x78 | Drawbar 16' .. 1' | lin 0..8 | ADR-0007 |

Every new ID is also accepted by `FastParamUpdate`. A full patch was 6 bytes of header, up to 11 for the name, 30 entries × 3 bytes and 2 bytes of trailer, so 98 + name length = 109 bytes at most. ADR-0007 adds 20 entries: 158 + name length = 169 bytes at most. Only `0x79..0x7E` remain free in the short ID space; further blocks move to the extended space.

## Firmware Changes
- **SysEx limit.** `src/ble_midi_server.h` declares the BLE-MIDI interface by hand instead of with `BLEMIDI_CREATE_INSTANCE`, using a settings struct derived from `BLEMIDI_NAMESPACE::MySettings` with `SysExMaxSize = 512`.
- **One parameter table.** `SysExProtocol`'s table of patch-scope `ParamId` → curve, range and `Preset` getter/setter holds all value scaling. It drives `PatchDumpV2` encode and decode, `RequestDump`, user-slot `FastParamUpdate` and live `FastParamUpdate` scaling. Live edits still need one `ParamId` → engine setter switch (`SynthEngine::applyLiveParam`), which has no scaling code. Global IDs keep their own handling. This replaces the two duplicated scaling switches in `SynthEngine::handleSysEx`.
- **Handshakes over BLE.** `SynthEngine::handleSysEx(data, len, reply)` returns each message's reply to its caller, written under the engine lock. `BleMidiServer` sends it with `MIDI.sendSysEx` after the call returns, so a BLE notify never runs under the lock, and replies to SysEx handled on other tasks (USB) can never go out over BLE.
- **Live engine mode and voicing.** `EngineMode` and `Voicing` updates set the engine mode and voicing directly; they do not re-apply the patch, so Casio tone, accent and vibrato are untouched. The engine keeps the patch's own voicing separate from the resolved one, so a generic patch switched to FM (DX voicing) and back gets its generic voicing back.
- **Global IDs** in `FastParamUpdate` apply only when the update targets the live sound (slot 0 or the active user slot), as before.
- **`RequestDump` (`0x04`)** replies with `PatchDumpV2`.
- **Legacy decoding.** `0x02` (62-byte v1) dumps are still decoded, so existing senders keep working; the firmware no longer encodes v1.

## Storage
- **New key.** A user slot is stored as the dump body (`nameLen`, `name`, entries) under the new NVS key `q_<slot>`, read with `getBytesLength`.
- **Loading.** If `q_<slot>` is absent, the 48-byte v1 blob under `p_<slot>` is decoded with the legacy decoder. The slot is written to `q_<slot>` on its next save, so nothing is rewritten at boot.
- **Downgrading.** `p_<slot>` is left in place. Flashing older firmware therefore restores each slot as it was last saved under v1, rather than resetting it to defaults.
- **Upgrading again.** Each `q_<slot>` write also stores `h_<slot>`, an FNV-1a fingerprint of the `p_<slot>` blob present at the time. If older firmware has since saved a different `p_<slot>`, the fingerprints differ and that newer v1 patch is loaded instead of the stale `q_<slot>` (until the slot's next save).
- **Corrupt blobs.** A `q_<slot>` that fails validation, or is larger than `MAX_PATCH_BODY_SIZE`, is ignored; the slot falls back to `p_<slot>` if present, else to its default patch.
- **Space.** Each slot takes at most 4–5 NVS entries (32 bytes each), negligible against the NVS partition.

## Consequences
- **No field reuse.** A value has one meaning in every engine, and FM patches keep tremolo rate and their own subtractive settings.
- **Compatibility.**
  - **Newer patches on an older v2 firmware:** they load, with unknown IDs skipped, so a patch sounds as close as that firmware allows.
  - **Older patches on newer firmware:** missing IDs take defaults.
  - **v2 senders on pre-v2 firmware:** the dump is rejected, and since pre-v2 firmware sends no replies over BLE, the sender sees no answer at all. A sender should treat a missing reply as "firmware older than ADR-0006".
- **Precision.** Values drop from 16 to 14 bits, matching what the live knob stream has always used. 16383 steps is under 1 cent per step on the cutoff curve and under 0.06 % on envelope times. Glide improves from 8 to 14 bits.
- **Size.** A full dump is 98–109 bytes today, up from 62. Over BLE this is a few packets and a few milliseconds.
- **Code and docs touched:**
  - `SysExProtocol.{h,cpp}` and `SynthEngine.cpp`.
  - `src/ble_midi_server.h`.
  - `tests/test_sysex.cpp`, `tests/test_hardware_drivers.cpp`, `tests/test_dsp_fidelity.cpp`, plus new tests for unknown-ID skipping, missing-ID defaults and v1 NVS migration.
  - `tools/patch_sender.html`.
  - `CONTEXT.md`, ADR-0001 and ADR-0002 (mark the superseded parts), `diagnostics/README.md`.
- **Hardware check needed.** The raised SysEx limit and BLE handshakes need checking on the board; a desktop build cannot exercise the BLE-MIDI library.
- **Large replies need a negotiated MTU.** BLE-MIDI 2.2 sends 64-byte packets, and its ESP32 class is tied to the default transport settings, so the packet size cannot be lowered without patching the library. A 7-byte handshake always fits, but a `RequestDump` reply (98–109 bytes) is truncated if the central keeps the default 23-byte ATT MTU. macOS and iOS negotiate a larger MTU; check on the board before relying on `RequestDump` over BLE.
- **BLE receive back-pressure.** The library's receive queue holds 64 bytes, so a dump longer than that blocks the BLE stack task until `loop()` drains it. Nothing is lost; the transfer just paces itself to `loop()`.
