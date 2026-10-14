# Domain Context: Polyphonic Synthesizer System

## Overview

A dedicated, real-time polyphonic synthesizer system specifically targeting the Akai MPK Mini MK4 hardware controller, supporting desktop simulation (macOS CoreAudio/PortAudio) and embedded hardware (ESP32-S3 Rev 2.0 with USB Host MIDI, BLE SysEx Patch Update Server, SSD1306 OLED, and PCM5102A I2S DAC).

## Core Domain Concepts

### SynthEngine
The central audio and polyphony coordinator. Manages voice allocation, voice stealing (LRU), MIDI message routing, active preset parameters, and audio output rendering. In **deferred event mode** (`setDeferredEvents(true)`, used on the ESP32-S3 and in the simulator) note events and voice parameter pushes are queued lock-free and applied by the audio thread, so only the audio thread touches `Voice` state.

### Model Voicing
The hardware character of each modelled instrument, separate from patch parameters (`ModelVoicing` / `getModelVoicing()` in `Presets.h`): filter topology, filter keyboard tracking, velocity response, TB-303 accent, oscillator key-sync, per-note analog drift, and original polyphony (Juno-60 = 6, Prophet-5 = 5, Minimoog/TB-303 = mono…). A preset's `voicing` field (persisted in SysEx patch flags bits 4..7) lets a user patch keep the character of the factory model it was saved from.

### Voice
A self-contained sound generator containing:
- **Oscillator 1 & 2**: Free-running band-limited PolyBLEP oscillators with detune (phase resets only for key-synced digital/FM voices starting from silence).
- **Sub-Oscillator**: Square wave one octave below, phase-locked to Oscillator 1 (divide-by-two, as on the Juno DCO).
- **Overdrive**: Soft-clipping saturation stage before the filter.
- **Filter**: Per-model topology: 24 dB/oct zero-delay-feedback transistor ladder with input saturation (self-oscillates at maximum resonance) or 12 dB/oct Cytomic SVF with continuous state saturation. Cutoff = base × envelope × keyboard tracking × TB-303 accent sweep, updated at control rate with a per-sample ramp.
- **Amp Envelope & Filter Envelope**: Exponential RC analog-style ADSR contour generators (attack reaches peak in the set time; decay/release cover 95% of their distance in the set time).
- **Pitch**: Tracked in semitones, so portamento is symmetric like an RC lag on a 1 V/oct CV.

### StereoChorus
Two bucket-brigade (BBD) delay lines driven in antiphase by one triangle LFO, followed by ~9 kHz BBD low-pass filters, modelling the Roland Juno-60 chorus unit. Dry/wet gains are power-normalised so the mix knob changes texture, not loudness.

### MidiParser
Streaming byte-by-byte finite state machine that decodes raw serial or USB bytes into structured MIDI events (Note On/Off, Control Change, Pitch Bend, Program Change) with full MIDI running status support.

### Preset
A tuned sonic parameter template modeling multiple hardware synthesizer families in a 32-slot bank where **Factory ROM Presets (`Slots #1..#23`)** sit contiguously at the front and all remaining capacity (`Slots #24..#32`) is reserved as **User Customizable Slots (`User_01..User_09`)** ([ADR-0007](./docs/adr/0007-additional-synthesis-engines.md)):
- **Classic Subtractive Analog (`Slots #1..#6`)**:
  1. Roland Juno-60
  2. Moog Minimoog Model D
  3. Sequential Prophet-5
  4. Roland TB-303 Acid
  5. Yamaha CS-80 Blade
  6. Oberheim OB-Xa Brass
- **Casio PT-10 Lo-Fi Digital (`Slots #7..#10`)**:
  7. Casio PT-10 Piano (Snappy Decaying Staccato Key Strike)
  8. Casio PT-10 Fantasy (Square + Octave Harmonic Lead)
  9. Casio PT-10 Violin (Narrow Multipulse Bowed String Swell)
  10. Casio PT-10 Flute (Mellow Lowpass Filtered Square)
- **2-Operator Polyphonic FM Keyboard (`Slots #11..#15`)**:
  11. Yamaha DX7 Classic Electric Piano
  12. FM Crystal Tines
  13. FM Warm Rhodes / Suitcase Tremolo
  14. FM Clavinet D6
  15. FM Drawbar Jazz Organ
- **Wavetable (`Slots #16..#17`)**:
  16. PPG Wave 2.3 (digital wavetable sweep into a 4-pole filter)
  17. Wave Vox (Waldorf Microwave style vowel-morph pad)
- **Casio CZ Phase Distortion (`Slots #18..#19`)**:
  18. CZ Bass (saw, snappy DCW envelope)
  19. CZ Reso (resonance wave lead, two detuned lines)
- **Karplus-Strong Plucked String (`Slots #20..#21`)**:
  20. KS Guitar (steel string)
  21. KS Koto (bright, near-bridge pick)
- **Additive Drawbar Organ (`Slots #22..#23`)**:
  22. B3 Jazz (888000000, third percussion, slow Leslie)
  23. B3 Gospel (888808008, key click, fast Leslie)
- **User Customizable Bank (`Slots #24..#32`)**:
  24–32. `User_01`..`User_09` (Non-volatile NVS-backed custom patches)

Each preset exposes 16 parameters split across **Bank A (Primary Knobs 1..8)** and **Bank B (Knobs 9..16, selected with BANK A/B)**, discriminated by `EngineMode` (`Subtractive`, `FM`, `Wavetable`, `PhaseDist`, `Pluck`, `Additive`). Timbres and models can be selected directly via the Akai MPK Mini IV's **Main Push-Turn Navigation Knob** or front-panel **Left / Right Cursor Keys (`<` / `>`)**.

### Synthesis Engines
Selected per patch by `EngineMode`; the engine-specific parameters live in `TimbreParams` (`Parameters.h`), rendered by `Voice` (`Voice.cpp`, `VoiceEngines.cpp`):
- **Subtractive**: two PolyBLEP oscillators + sub → drive → ladder / SVF filter → amp.
- **FM**: 2-operator phase modulation with modulator self-feedback; the filter envelope is the modulator envelope.
- **Wavetable**: two detuned oscillators reading band-limited, mip-mapped single-cycle frames (`Wavetables.h`: 4 tables × 8 frames, built at boot into PSRAM) and morphing between frames; the filter envelope sweeps both cutoff and frame position, then the subtractive filter.
- **Phase distortion (Casio CZ)**: a cosine read through a warped phase (saw, square, pulse, double sine, three resonance waves) by one or two detuned lines; the filter envelope is the DCW envelope. No filter.
- **Pluck (Karplus-Strong)**: a shaped noise burst (brightness, pick position) recirculating in a per-voice delay line tuned by a fractional-delay allpass, with a one-zero damping filter and a loop gain set from the string decay time.
- **Additive (Hammond)**: nine drawbars on harmonics 1..16 of the 16' pitch, generated from one sine/cosine lookup by the Chebyshev recurrence; third/second-harmonic percussion on the filter envelope, key click, and the engine tremolo as the Leslie.

### Hardware Drivers & Targets
An adapter layer bridging the platform-agnostic `SynthEngine` and `MidiParser` with target-specific peripherals:
- **ESP32-S3 Rev 2.0 (`src/esp32s3_driver.h`, `src/usb_host_midi.h`, `src/oled_ui.h`, `src/animations/`, `src/ble_midi_server.h`)**:
  - MCU: ESP32-S3-DevKitC-1 N16R8 (16MB Flash, 8MB Octal PSRAM).
  - DAC: PCM5102A 32-bit I2S Stereo DAC (`i2s_std` driver, Core 1 audio DSP).
  - Controller: Akai MPK Mini MK4 direct connection via Native USB Host (GPIO 19/20).
  - Display: 0.96" I2C SSD1306 OLED (GPIO 8 SDA, GPIO 9 SCL) at 30 FPS with pluggable `IAnimation` visualizers in `src/animations/`.
  - Fault display (`src/system_status.h`): `SystemStatus` holds a `SystemFault` bitmask set by boot code and loop() watchers, logged once per transition to Serial. Fatal faults (I2S DAC / audio task init) replace the OLED with a full-screen fault page; warnings (MK4 not on USB Host, Plugin Port silent for 3 s, NVS, BLE, PSRAM) show an inverted header tag plus a 3 s banner. Until the MK4 first enumerates the blue zone shows "Connect MPK Mini MK4". Nothing runs on the audio task.
  - Wireless: BLE SysEx Server with non-volatile NVS patch persistence (keys `q_16..q_32`, legacy `p_16..p_32`). Standard performance MIDI input is routed via Native USB Host.
  - Volume: Master volume is adjusted from the MPK Mini MK4 knob (K8, CC 7); no analog pot is fitted.
  - Schematics & Wiring: Documented in [`HARDWARE.md`](./HARDWARE.md).
- **Desktop Simulator (`sim/main_sim.cpp`)**: PortAudio double-buffered callback (16-bit stereo @ 44.1 kHz) + RtMidi MPK Mini MK4 auto-detection + SysEx OLED display driver.
- **Host Test Drivers (`tests/mock_hal/`)**: Native host-executable mock peripheral driver enabling verification of DMA buffers, serial MIDI streams, OLED UI / animations (`U8g2lib.h`), and NVS storage on macOS.

### SysEx Protocol (`lib/SynthEngine/include/SysExProtocol.h`)
Manufacturer-specific MIDI System Exclusive (`F0 7D ... F7`) byte protocol facilitating bidirectional, real-time patch updates and persistence:
- **Fast Parameter Streaming**: 10-byte fire-and-forget messages (`F0 7D 01 01 <ParamID> <ValMSB> <ValLSB> <Slot> <Reserved> F7`) throttled at 30 Hz for zero-latency live knob modulation directly into active RAM or User Slots (`#24..#32`) without flash wear.
- **Full Patch Dump & Storage** ([ADR-0006](./docs/adr/0006-parameter-list-patch-format.md)): `PatchDumpV2` (`F0 7D 01 05 <slot> <nameLen> <name> {<ParamID> <ValMSB> <ValLSB>}... <xor> F7`) is a list of 14-bit parameter entries using the same IDs and encoding as the fast stream, about 110 bytes for a full patch. Unknown IDs are skipped and missing IDs take their defaults. Each dump is answered with an ACK/NACK over BLE; slot 0 auditions in the live buffer, slots `#24..#32` are saved to NVS under `q_<slot>` (falling back to the legacy 48-byte `p_<slot>` blob when absent). Legacy `0x02` 62-byte dumps are still accepted.

### MpkMiniMk4ControlSurface (`lib/MpkMiniMk4Display/include/MpkMiniMk4ControlSurface.h`)
The bidirectional hardware interaction module governing the Akai MPK Mini MK4 control surface:
- **Knob Value Pre-Seeding (`SysEx 0x12`)**: Writes 10-byte `F0 47 00 5D 12 00 02 <k> <val> F7` messages over `Plugin Port` to seed the keyboard's internal knob accumulators on preset load, Bank A/B switches, and external parameter changes, eliminating value drops to 0.
- **Echo Suppression**: Suppresses echoing `SysEx 0x12` back to the hardware encoder when parameter changes originate from physical knob turns (`ChangeSource::PhysicalKnob`).
- **Dual-Bank Cache**: Maintains live session values for Bank A (`K1..K8`) and Bank B (`K9..K16`) across `BANK A/B` toggles (`SHIFT` + knob is the MK4 firmware's fine adjust, delta/4, and never changes bank).
- **Surface Gestures**: Intercepts `SHIFT + Keybed Note` for root key selection, `SHIFT + Nav Dial` for ±4 preset stepping, `SHIFT + BANK` for the Rhythm page (metro level, tempo, time sig, beats/bar, click WOOD/C..B, STEREO/MONO output saved in NVS, SOUND/SILENT metronome, master volume), and `-`/`+` navigation. Scale quantizing is left to the MK4's own Scale mode.
- **Plugin Port Connection Requirement**: Enforces active `Plugin Port` SysEx communication; halts parameter updates and alerts the user if `Plugin Port` is disconnected.

### Hardware Diagnostics (`diagnostics/`)
Standalone verification sketches for isolating hardware faults:
- `01_system_health`: CPU clock, FreeRTOS core affinity, PSRAM allocation.
- `02_i2s_dac_test`: Pure 440 Hz stereo sine tone via PCM5102A DAC.
- `03_i2c_oled_test`: SSD1306 128x64 display rendering and frame timing.
- `05_usb_mpk_test`: USB Host MIDI enumeration and packet decoding for MPK Mini MK4 (284-byte SysEx).
- `06_ble_midi_nvs_test`: BLE advertising and NVS flash read/write cycles for wireless patch dumps.

