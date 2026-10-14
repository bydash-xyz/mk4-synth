# Additional Synthesis Engines and a Smaller User Bank

**Status: Accepted.** Changes the slot partition of ADR-0002 (factory `#1..#15`, user `#16..#32`) and extends the parameter table of ADR-0006.

The engine adds four synthesis methods next to subtractive and 2-operator FM, each with two factory presets. To make room, the 32-slot bank keeps its size and the user bank shrinks from 17 to 9 slots.

| Slots | Engine | Presets |
|---|---|---|
| 16..17 | Wavetable (PPG Wave / Waldorf Microwave) | PPG WAVE, WAVE VOX |
| 18..19 | Phase distortion (Casio CZ-101) | CZ BASS, CZ RESO |
| 20..21 | Karplus-Strong plucked string | KS GUITAR, KS KOTO |
| 22..23 | Additive drawbar organ (Hammond B-3) | B3 JAZZ, B3 GOSPEL |
| 24..32 | User slots `User_01..User_09` | |

## Considered Options
1. **Shrink the user bank (chosen).** Factory presets stay contiguous at the front and the bank stays at 32 slots, so slot-based code (`isUserSlot`, NVS keys `q_<slot>`, preset stepping) keeps its shape. Patches saved in slots 16..23 are no longer reachable.
2. **Grow the bank and renumber the user slots.** Keeps every user slot but needs a one-time NVS key migration and moves every user slot number in SysEx, tools and docs.
3. **Append factory presets after the user slots (`#33+`).** No migration, but factory presets would no longer be contiguous at the front.

## Decisions
- **Engines.** `EngineMode` gains `Wavetable`, `PhaseDist`, `Pluck` and `Additive`. Their parameters are a `TimbreParams` block in `Preset`. Each engine also uses the shared patch fields: the amp envelope; the filter envelope, which sweeps the wavetable position, the CZ DCW and the organ percussion; detune for the second wavetable oscillator or CZ line; drive; chorus; and tremolo, which is the organ's Leslie.
- **Factory model indices 0..14 do not change.** The new models take indices 15..22 and the user models 23..31. v1 (`0x02`) dumps and `p_<slot>` blobs were written when indices 15..31 were user models, so the decoder reads a v1 model byte of 15 or more as `User`. The 4-bit voicing field of v1 still holds models 0..14 only; PatchDumpV2's `Voicing` ID covers all factory models.
- **New parameter IDs** `0x65..0x78` (see ADR-0006). `EngineMode` becomes a six-value enum. Senders that send 1 for FM keep working.
- **New knob CCs** `40..49`, `51`, `53..63`: the MIDI LSB numbers of controllers that no MK4 control sends, clear of the MK4's hardware CCs. `53` / `54` set tremolo depth / rate in every engine.
- **CPU and memory.** Phase distortion and pluck cost less per voice than the subtractive voice, the organ about as much as FM, and the wavetable voice about 10 % more than an OB-Xa voice (measured on the desktop; the ESP32-S3 profile build gives the on-device numbers). Wavetables are built at boot into one 200 KB block, which the ESP32-S3 allocator places in PSRAM; each voice has a 4 KB pluck delay line.
- **Band-limiting.** Wavetable frames carry at most 63 harmonics in 512 samples at six mip levels, and voices read the richest level below Nyquist. Organ partials above Nyquist are dropped. Phase distortion reduces its DCW as pitch rises, as the CZ's key follow did. Measured inharmonic (alias) energy stays below −50 dB, except for the CZ resonance waves above C6, which keep the hard window corners of the original and stay below −30 dB.
