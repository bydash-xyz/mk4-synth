# Akai MPK Mini IV (MK4) — Ground-Truth USB-MIDI & SysEx Specification

> **Source**: Extracted directly from the connected **Akai MPK Mini IV** (`USB Manufacturer ID = 0x47`, `Device / Model ID = 0x5D`) via CoreMIDI / `RtMidi` SysEx RAM & Preset Slot queries (`F0 47 7F 5D 66 00 00 <slot> F7` $\rightarrow$ `F0 47 7F 5D 67 02 14 ... F7`, 284 bytes).

---

## 1. USB-MIDI Port Topology (4 Input / 4 Output Ports)

When connected over USB-C, the Akai MPK Mini IV enumerates **4 bidirectional USB-MIDI ports**:

| Port Index | Port Name | Direction | Purpose |
| :--- | :--- | :--- | :--- |
| **`0`** | `MPK mini IV MIDI Port` | IN / OUT | Primary performance MIDI port (25-key keybed on Ch 1, 8 MPC pads on Ch 10, 8 rotary knobs `CC 24..31`, Pitch/Mod wheels). |
| **`1`** | `MPK mini IV DAW Port` | IN / OUT | Dedicated DAW control surface port (Transport buttons, navigation encoder/cursor buttons, `SHIFT` modifier events). |
| **`2`** | `MPK mini IV Plugin Port` | IN / OUT | Plugin host control port (mirrors plugin preset CCs). |
| **`3`** | `MPK mini IV Software Control Port` | IN / OUT | Dedicated **SysEx Management & OLED Screen Port** (used for 284-byte RAM preset read/write `0x66`/`0x67`/`0x64` and live OLED text line writes `0x6B`/`0x68`/`0x6A`). |

---

## 2. Ground-Truth Hardware Controls & MIDI Mapping (`Slot 0..8`)

Across **all 9 hardware preset slots** (`Slot 0` Active RAM, `Slot 1` `DAW`, `Slot 2` `PLUGIN`, `Slot 3..8` `USER1..USER6`), the physical controls map as follows:

### A. 8 Continuous 360° Rotary Encoders (`K1..K8` $\rightarrow$ `CC 24..31`)

| Physical Knob | Position on Hardware | Native MIDI CC | Hex | Mode | Range | Default Hardware Label | Synth Bank A (`BANK A/B` = A) | Synth Bank B (`BANK A/B` = B) |
| :--- | :--- | :---: | :---: | :--- | :---: | :--- | :--- | :--- |
| **`K1`** | Top Row, Knob 1 (Left) | **`CC 24`** | `0x18` | `0` (Absolute) | `0..127` | `KNOB1` / `Cutoff` | **Filter Cutoff** (`CC 74`) | **Filter Env Amt** (`CC 76`) |
| **`K2`** | Top Row, Knob 2 | **`CC 25`** | `0x19` | `0` (Absolute) | `0..127` | `KNOB2` / `Resonnc` | **Filter Resonance** (`CC 71`) | **Filter Attack** (`CC 77`) |
| **`K3`** | Top Row, Knob 3 | **`CC 26`** | `0x1A` | `0` (Absolute) | `0..127` | `KNOB3` / `SubOsc` | **Sub-Osc / Vibrato** (`CC 72`) | **Filter Decay** (`CC 78`) |
| **`K4`** | Top Row, Knob 4 (Right) | **`CC 27`** | `0x1B` | `0` (Absolute) | `0..127` | `KNOB4` / `Chorus` | **Chorus / Tone** (`CC 91`) | **Filter Sustain** (`CC 82`) |
| **`K5`** | Bottom Row, Knob 5 (Left) | **`CC 28`** | `0x1C` | `0` (Absolute) | `0..127` | `KNOB5` / `Attack` | **Amp Attack** (`CC 73`) | **Filter Release** (`CC 83`) |
| **`K6`** | Bottom Row, Knob 6 | **`CC 29`** | `0x1D` | `0` (Absolute) | `0..127` | `KNOB6` / `Decay` | **Amp Decay** (`CC 75`) | **Chorus Mix** (`CC 91`) |
| **`K7`** | Bottom Row, Knob 7 | **`CC 30`** | `0x1E` | `0` (Absolute) | `0..127` | `KNOB7` / `Sustain` | **Amp Sustain** (`CC 70`) | **Osc Detune** (`CC 84`) |
| **`K8`** | Bottom Row, Knob 8 (Right) | **`CC 31`** | `0x1F` | `0` (Absolute) | `0..127` | `KNOB8` / `Release` | **Amp Release** (`CC 72`) | **Master Volume** (`CC 7`) |

> [!IMPORTANT]
> **1. `DAW Port` (`Slot 1`) vs `Plugin / MIDI Port` (`Slot 0` & `Slot 2..8`)**:
> - **`Slot 1 (DAW)` (`MPK mini IV DAW Port`)**: Configures all 8 knobs `K1..K8` (`CC 24..31`) with **`b3(mode) = 1` (Relative 2's-Complement Endless Encoder Mode)**—sending `1..15` (`+1..+15`) when turned clockwise and `127..113` (`-1..-15`) when turned counter-clockwise.
> - **`Slot 2 (PLUGIN)` & `Slot 0 (MK4-SYNTH)` (`MPK mini IV MIDI Port` / `Plugin Port`)**: Configures all 8 knobs `K1..K8` (`CC 24..31`) with **`b3(mode) = 0` (Absolute `0..127` Mode)**—sending continuous values `0..127`.
> - **Both Modes Supported**: a host can listen to **`MIDI Port`**, **`DAW Port`**, and **`Plugin Port`** simultaneously and accumulate relative 2's-complement steps (`1..15` / `127..113`) when `CC 24..31` arrives on `MPK mini IV DAW Port`.
>
> **2. Why `syncMpkMiniKnobNames()` Previously Broke the Knobs**:
> Previously, `syncMpkMiniKnobNames()` in `tools/mpk_display.cpp` wrote `CC 70..77` into the MPK Mini IV's 284-byte RAM preset (`offset 117 + k*20`), overwriting the hardware's native `CC 24..31` (`0x18..0x1F`). `mpk_display.cpp` now preserves `CC 24..31` (`mode = 0`) when updating the 16-character ASCII knob names on the MPK Mini IV OLED screen.

### B. 25-Key Velocity-Sensitive Keybed & Performance Wheels

| Control | MIDI Channel | MIDI Message | Default Range / Notes |
| :--- | :---: | :--- | :--- |
| **25-Key Keybed** | Channel 1 (`0x90` / `0x80`) | Note On / Note Off | **Leftmost Key = Note `#48` (`C3`)** $\rightarrow$ **Rightmost Key = Note `#72` (`C5`)** at default Octave `0` |
| **Pitch Bend Wheel** | Channel 1 (`0xE0`) | 14-bit Pitch Bend (`LSB, MSB`) | Center = `8192` (`0x00, 0x40`), Range = `0..16383` ($\pm 2$ semitones) |
| **Modulation Wheel** | Channel 1 (`0xB0`) | **`CC 1`** | `0..127` (routed strictly to LFO Vibrato & Tremolo depth, never Filter Cutoff) |
| **Sustain Pedal Input** | Channel 1 (`0xB0`) | **`CC 64`** | `0` = Released, `127` = Pedal Down |

### C. 8 RGB Velocity-Sensitive MPC Pads (Bank A & Bank B)

| Pad (Bank A / Bank B) | MIDI Channel | Note Number | Program Change (`PROG CHNG`) | Control Change (`CC`) |
| :--- | :---: | :---: | :---: | :---: |
| **Pad 1 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `36` (`C1`)** | `PC 32` | `CC 0` |
| **Pad 2 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `37` (`C#1`)** | `PC 33` | `CC 1` |
| **Pad 3 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `38` (`D1`)** | `PC 34` | `CC 2` |
| **Pad 4 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `39` (`D#1`)** | `PC 35` | `CC 3` |
| **Pad 5 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `40` (`E1`)** | `PC 36` | `CC 4` |
| **Pad 6 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `41` (`F1`)** | `PC 37` | `CC 5` |
| **Pad 7 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `42` (`F#1`)** | `PC 38` | `CC 6` |
| **Pad 8 (Bank A)** | Channel 10 (`0x99` / `0x89`) | **Note `43` (`G1`)** | `PC 39` | `CC 7` |
| **Pads 9..16 (Bank B)** | Channel 10 (`0x99` / `0x89`) | **Notes `44..51` (`G#1..D#2`)** | `PC 40..47` | `CC 8..15` |

### D. Navigation, Bank & Modifier Controls (`src/usb_host_midi.h`)

| Hardware Control | MIDI CC | Value | Action in `UsbHostMidi` (`src/usb_host_midi.h`) |
| :--- | :---: | :---: | :--- |
| **Push-Turn Encoder (Dial)** | `CC 14` (hardware) / `CC 72` | Relative: CW `1..63`, CCW `127..65` | Steps active synth preset (±n, clamped to 4, or `±4` with `SHIFT`) |
| **Push-Turn Encoder (Click)** | `CC 13` (hardware) / `CC 15` (custom map) | `127` | Refreshes MPK screen |
| **`-` / `<` (Prev Preset)** | `CC 80` / `CC 74` / `CC 16` | `127` | Previous Preset (`SHIFT` + `-` = previous OLED animation) |
| **`+` / `>` (Next Preset)** | `CC 81` / `CC 71` | `127` | Next Preset (`SHIFT` + `+` = next OLED animation) |
| **`SHIFT` Modifier Button** | **`CC 17`** / `CC 18` | `127` (Held) / `0` (Released) | Never switches bank. Firmware **fine-adjust**: while held, knob deltas are divided by 4 on the device (precise tuning). Bank is chosen only by `BANK A/B` (CC 12) |
| **`BANK A/B` Button** | **`CC 12`** / `CC 50` | `127` (Press) / `0` (Release) | Toggles and latches **Bank A/B** (Bank A `K1..K8` $\leftrightarrow$ Bank B `K9..K16`) |
| **`TAP TEMPO` (`METRO`)** | `CC 20` / `CC 119` | `127` | Tap Tempo (`SHIFT` + `TAP TEMPO` or `CC 19` / `CC 118` toggles Metronome) |

---

## 3. 284-Byte SysEx RAM Preset & OLED Screen Protocol (`Model ID = 0x5D`)

All SysEx messages use the Akai header `F0 47 7F 5D <cmd> <len_hi> <len_lo> ... F7` over **`MPK mini IV Software Control Port`**:

1. **Query Preset Slot (`0x66`)**:
   - Request: `F0 47 7F 5D 66 00 00 <slot> F7` (`slot = 0x00` for Active RAM, `0x01..0x08` for Preset Slots 1..8).
   - Response (`0x67`, **284 bytes**):
     - `Bytes 0..7`: `F0 47 7F 5D 67 02 14 <slot>`
     - `Bytes 8..23` (16 bytes): ASCII Preset Name (e.g. `"DAW"`, `"PLUGIN"`, `"MK4-SYNTH"`)
     - `Bytes 24..35` (12 bytes): Keybed Ch (`offset 24`), Pad Ch (`offset 25`), Octave (`offset 26`), Arp/Latch/Clock settings, Tempo (`offset 31`)
     - `Bytes 36..115` (80 bytes): 16 Pad Descriptors (`16 × 5 bytes`: `[Note, PC, CC, Mode, Color]`)
     - `Bytes 117..276` (160 bytes): **8 Knob Descriptors (`8 × 20 bytes`)**:
       - `offset + 0`: MIDI CC Number (`24..31` = `0x18..0x1F`)
       - `offset + 1`: Min Value (`0x00`)
       - `offset + 2`: Max Value (`0x7F` = `127`)
       - `offset + 3`: Knob Mode (`0x00` = Absolute `0..127`)
       - `offset + 4..19`: 16-byte ASCII Knob Name shown on the MPK Mini IV OLED when turned!
2. **Write Active RAM Preset (`0x64`)**:
   - Send the 284-byte buffer back with `Byte 4 = 0x64` and `Byte 7 = 0x00` (updates RAM & OLED labels immediately with zero flash wear).
3. **Direct Live OLED Text Commands (`0x6B`, `0x68`, `0x6A`)**:
   - Clear Line (`0x6B`): `F0 47 7F 5D 6B 00 02 <line 0..2> 00 F7`
   - Format Line (`0x68`): `F0 47 7F 5D 68 00 0B <line> <align> <fgR> <fgG> <fgB> <bgR> <bgG> <bgB> 00 00 00 F7`
   - Write Text (`0x6A`): `F0 47 7F 5D 6A 00 <len> <line> <col> <ASCII...> F7`
