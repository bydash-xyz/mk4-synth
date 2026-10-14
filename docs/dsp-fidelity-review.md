# Synth Engine Review — Bugs & Authenticity Gaps

**Reviewed:** 2026-09-28 · **Fixes:** landed the same day
**Scope:** `lib/SynthEngine`: Oscillator, Filter, Envelope, Voice, Chorus, SynthEngine, Presets
**Goal:** Sound and feel like the modelled instruments (Juno-60, Minimoog, Prophet-5, TB-303, CS-80, OB-Xa, Casio PT-10, DX-style FM). Polyphony is a nice-to-have.

**Regression tests:** [`tests/test_dsp_fidelity.cpp`](../tests/test_dsp_fidelity.cpp) (`ctest -R DspFidelityTest`) has one check per fixed item.

Status key: ✅ fixed · 🟨 partly fixed · ⬜ open

---

## Summary

| ID | Issue | Status |
|---|---|---|
| E-01 | Filter state "soft saturation" snapped 4.0 → 2.83, which clicked | ✅ |
| E-02 | Oscillator phase reset on every note-on (clicks; identical attacks) | ✅ |
| E-03 | Mono High/Low priority released a note that was still held | ✅ |
| E-04 | TB-303 slide retriggered the envelopes | ✅ |
| E-05 | Audio thread rendered voices while Core 0 changed them | ✅ |
| E-06 | Envelope attack ran at 37% of the set time | ✅ |
| E-07 | Triangle started with a DC offset and clipped | ✅ |
| E-08 | Glide was exponential in Hz (up/down asymmetric 4.4×) | ✅ |
| E-09 | One knob drove two parameters (Sub+Drive, Chorus+Detune, VibRate+VibDepth) | ✅ |
| E-10 | Knob labels without implementation (PolyMod, Accent, Distort, PolyMode, Glide) | ✅ |
| E-11 | Master soft-clip compressed chords by 6 dB | ✅ |
| E-12 | Legato jumped the level to the new velocity | ✅ |
| E-13 | Engine switch left stale FM phase increments | ✅ |
| E-14 | FM phase could grow without bound at high ratios | ✅ |
| E-15 | 2-point PolyBLEP aliasing (−28 dB at 3.5 kHz) | ⬜ |
| E-16 | Filter cutoff stepped every 16 samples | ✅ |
| E-17 | Quantized notes released each other | ✅ |
| E-18 | Voice-mode switch hard-killed voices | ✅ |
| E-19 | Chorus raised the level +3.5 dB at full mix | ✅ |
| E-20 | Trio "Da Da Da" credited to the PT-10 | ✅ |
| A-01 | No 24 dB/oct ladder filter | ✅ |
| A-02 | No self-oscillation or resonance bass loss | ✅ |
| A-03 | No filter keyboard tracking | ✅ |
| A-04 | LFO routed to pitch only (no PWM / VCF) | ⬜ |
| A-05 | Fixed oscillator topology (Juno has 1 DCO, Minimoog 3 VCOs, Prophet sync / poly-mod) | ⬜ |
| A-06 | No noise source | ⬜ |
| A-07 | No TB-303 accent / slide | 🟨 |
| A-08 | Chorus not Juno-like | 🟨 |
| A-09 | Same envelope behaviour for every model | ⬜ |
| A-10 | Velocity always on | ✅ |
| A-11 | No analog drift or voice spread | ✅ |
| A-12 | Voice counts don't match the originals | ✅ |

---

## What changed, by item

### E-01 — Continuous filter saturation ✅
**Before:** at Q=25, the maximum sample-to-sample step was 1.19 against a smooth bound of 0.59.
**After:** the SVF uses `8·tanh(s/8)` on both integrator states every sample (about 2% colour at normal levels). The new ladder saturates its input with `2·tanh(u/2)`. Both use the Padé `ParameterScaler::fastTanh`, which joins its ±1 clamp with zero slope. `Filter.cpp`.
**Test:** `testFilterSaturationIsSmooth` (SVF and ladder).

### E-02 — Free-running oscillators ✅
Oscillators, the filter and the accent state reset **only when a voice starts from silence**. A sounding voice keeps its phase, and the envelope continues from its current level, so retriggers and steals are click-free. **Before:** retrigger step 0.027 against a steady 0.0034. **After:** 0.0028 against 0.0025. Phase reset now happens only for voicings with `oscKeySync` (Casio, FM), and only from idle. Voices start at random phases. `Voice::noteOn`.
**Test:** `testRetriggerIsClickFree`.

### E-03 — Mono note priority ✅
The spurious raw-note release in `noteOff` is gone. `noteOnMono` keeps the sounding note when priority says so, and the held-key stack stores the physical key. `SynthEngine.cpp` `noteOnMono`/`noteOffMono`.
**Tests:** `testMonoPriorityKeepsHeldNote`, `testScaleChangeNoStuckNotes`.

### E-04 / A-07 — TB-303 slide and accent 🟨
- Legato is decided by the `legatoEnabled` flag only. The PT-10 Piano retriggers because its legato is off, not because of the removed sustain > 0.05 rule.
- The 303 VCA is now gate-held with a long fixed decay: 3 ms attack, 1.2 s decay, 25 ms release (`Presets.h`).
- **Accent:** a 303-voiced note at velocity ≥ 100 is louder (up to +60%). It also charges an "accent sweep" RC (40 ms) from the filter envelope, which adds up to 3 octaves × `Accent` to the cutoff and stacks up over consecutive accents. The `Accent` knob is CC 88 (`CC_ACCENT`).
- **Still open:** accent doesn't shorten the MEG decay (on a real 303 the filter decay goes to its minimum on accented steps). The 303 filter is the 4-pole ladder, not a diode ladder (A-01 note).

**Test:** `testTb303SlideAndAccent`.

### E-05 — Thread-safe engine ✅
`SynthEngine::setDeferredEvents(true)` makes note on/off, sustain, all-notes-off, panic and fast-release go through a lock-free SPSC ring (256 events). Parameter setters mark the voices dirty instead of touching them. `renderInternal()` drains both at the top of each block. Producers are already serialised by `engineMutex_`. Enabled in `src/esp32s3_driver.h` and `sim/main_sim.cpp`. Unit tests keep the immediate default. The mono held-key vector is pre-reserved, so the audio thread never allocates.
**Tests:** `testDeferredEvents`, `test_esp32s3_driver.cpp`.

### E-06 — Envelope timing ✅
Attack now reaches peak in exactly the set time (charging toward the 1.5× overshoot). Decay and release keep their documented "95% of the distance in the set time" definition. This was already true, so decay/release voicing is unchanged. Sustain changes now slew. A new `Envelope::fastRelease()` is used for mode and engine switches.
**Note:** attacks are now about 2.7× slower than before *for the same setting*, which is what the numbers always claimed (for example, the CS-80's 350 ms swell really is 350 ms now).
**Test:** `testEnvelopeAttackTiming`.

### E-07 — Triangle ✅
`resetPhase()` seeds the integrator on the ideal triangle for that phase. First cycle: peak 0.99, DC −0.005 (was 1.20 clamped, DC +0.48).
**Test:** `testTriangleStartup`.

### E-08 — Pitch-domain glide ✅
Voices track pitch as a fractional note. Glide is a one-pole lag in semitones, and frequency is computed with one `exp2` per 8-sample control tick. **After:** 138.6 ms both ways (was 60.6 up, 268.5 down). Vibrato no longer recomputes oscillator frequencies every sample (the old CRIT-04 perf issue).
**Test:** `testGlideSymmetry`.

### E-09 / E-10 — One knob, one parameter ✅
New CCs in `MidiMapping.h`: `CC_DRIVE` 87, `CC_ACCENT` 88, `CC_LFO_RATE` 89, `CC_LFO_DEPTH` 90, `CC_VOICE_MODE` 92 (<64 mono / ≥64 poly), `CC_GLIDE` 93 (0 = off, otherwise on + time). Detune uses the existing CC 94.

| CC | Now does only |
|---|---|
| 77 | Sub level (FM: feedback) |
| 78 | Chorus |
| 84 / 85 | Chorus rate / depth |

The preset knob maps now point at these CCs, with default knob positions recomputed from the preset values. Prophet `PolyMod` was relabelled `ModRate` (LFO rate), because poly-mod isn't implemented (A-05). The HUD (`describeParameter`) and manual generator were updated, and `docs/mpk_mini_mk4_manual.html` regenerated.
**Test:** `testKnobsAreDecoupled`, which also scans every factory knob label for its CC.

### E-11 — Gain staging ✅
Voices are summed linearly, trimmed by `kMasterBusTrim` (0.40), then passed through `ParameterScaler::outputLimiter`: exactly linear below 0.6, tanh knee above, never exceeding ±1. Single voices peak at 0.14–0.33 across the factory presets, and 8-note analog chords stay at ≤ 0.9. Full-velocity 8-note FM chords reach the limiter (about 0.98).
**Test:** `testOutputLimiter`.

### E-12 — Legato level ✅
`legatoNoteChange` keeps the phrase's velocity; only the accent flag updates per step.
**Test:** `testLegatoKeepsLevel`.

### E-13 … E-20 ✅
- **E-13:** `Voice::setEngineMode` recomputes increments. On a Sub↔FM switch, `applyPreset` fast-releases the old voices.
- **E-14:** FM phases wrap with `fmod`.
- **E-16:** the cutoff integrator gain ramps per sample across the 8-sample control interval.
- **E-17:** voices are allocated and released by the *physical* key (`rawNoteNumber`). Tests: `testQuantizedNoteOff`, `testScaleChangeNoStuckNotes`.
- **E-18:** `setVoiceMode` fast-releases (8 ms) instead of `panic()`. `renderInternal` renders every active voice, so tails outside the current mode or limit finish. Test: `testModeSwitchFades`.
- **E-19:** chorus dry/wet is power-normalised. Test: `testChorusLevel`.
- **E-20:** PT-10 Fantasy description fixed in `Presets.h`, `CONTEXT.md` and `sim/main_sim.cpp`.

### A-01 / A-02 — Ladder filter ✅
New `LadderFilter` (`Filter.h/.cpp`): a ZDF/TPT 4-pole transistor ladder (Zavalishin ch. 5) with tanh on the feedback-summed input. Resonance in the shared Q units maps to feedback through `ParameterScaler::resonanceQToLadderK` (the same square-root curve as the knob; k ≥ 4 self-oscillates in the top ~10%). There is 50% passband make-up, so resonance still thins the bass without large level jumps between presets. Used by the Juno, Minimoog, Prophet, 303, OB-Xa and generic user voicings. The CS-80 and Casio keep the 12 dB SVF.
**Test:** `testLadderCharacter` (sustained self-oscillation at max Q; passband drop at high Q).

### A-03 / A-10 / A-11 / A-12 — Model voicing ✅
`ModelVoicing` / `getModelVoicing()` in `Presets.h`:

| Model | Filter | Key track | Velocity | Drift | Voices |
|---|---|---|---|---|---|
| Juno-60 | ladder | 0.5 | 0 | 0.3 | 6 |
| Minimoog | ladder | 0.33 | 0 | 1.0 | 1 |
| Prophet-5 | ladder | 0.5 | 0 | 0.8 | 5 |
| TB-303 | ladder | 0 | 0 (accent) | 0.3 | 1 |
| CS-80 | SVF | 0.5 | 0.7 | 0.8 | 8 |
| OB-Xa | ladder | 0.5 | 0 | 0.8 | 8 |
| Casio | SVF | 0 | 0 | 0 | 1 (key-sync) |
| FM | – | – | 1.0 | 0 | 8 (key-sync) |
| User default | ladder | 0.5 | 0.5 | 0.5 | 8 |

- Drift gives each note ±3 ct × drift of pitch, an extra ±2 ct on osc 2, and ±3.5% cutoff.
- FM velocity → index is now partial (0.25 + 0.75·v²), so soft notes aren't pure sines.
- `Preset::voicing` records which factory character a patch uses. `snapshotActiveParameters` fills it and the SysEx flags carry it in bits 4..7, so a user slot saved from the Minimoog stays a Minimoog.
- `getEffectiveVoiceLimit()` = min(platform limit, model voices).

**Tests:** `testVoicing`, `testVoiceCounts`, `testVoicingRoundTrip`.

### A-08 — Chorus 🟨
One triangle LFO now drives the two delay lines in **antiphase** (was sine quadrature), and each is followed by a ~9 kHz BBD low-pass.
**Still open:** fixed Juno modes (I ≈ 0.5 Hz, II ≈ 0.8 Hz, I+II), a Juno-accurate delay range (the centre is 6 ms; the Juno is around 1.5–5 ms), and BBD noise/companding.

---

## Still open

| ID | What's missing | Notes |
|---|---|---|
| E-15 | Better anti-aliasing for the top octaves | 4-point BLEP/BLAMP or 2× oversampling above ~2 kHz fundamentals |
| A-04 | LFO → PW and LFO → cutoff, LFO shapes, delay/fade-in | Juno PWM is the core of its string sound. Needs knob slots (bank B has none free), so pair it with A-05 |
| A-05 | Per-preset oscillator config | Per-osc waveform, octave and level, and osc2 off. The Juno preset still layers a 6-cent detuned osc2, which a real single-DCO Juno can't do. The 303 only gets away with it because its detune is 0. Also Prophet hard sync and poly-mod, and the Minimoog's third oscillator |
| A-06 | Noise source | White/pink into the mixer |
| A-07 rest | 303 accent shortens MEG decay; diode-ladder character | Open303 (Robin Schmidt) is a good reference |
| A-08 rest | Fixed Juno chorus modes, delay range, BBD noise | |
| A-09 | Per-model envelope behaviour | Minimoog decay-as-release switch; Juno ADSR shared between VCF and VCA |
| FM | 2-op / one algorithm; no key-level scaling | 4-op with two stacks would reach DX E.Piano "body + tine" |
| CS-80 | HPF → LPF pair with its own envelopes; aftertouch brightness | The MPK MK4 keybed has no aftertouch; map to the mod wheel |

## Performance

Host (Apple Silicon, `-O2`), 8 voices with vibrato: about 0.8–1.0% of real time (was about 0.7%). The ladder and the per-sample filter coefficient ramp cost more, and per-sample `pow` from vibrato is gone. This hasn't been profiled on the ESP32-S3 yet. Check it with the diagnostic sketch before adding A-04/A-05, which add more per-sample work.

### Block voice renderer (branch `perf/voice-block-render`)

`Voice::renderBlock(acc, n, bend)` replaces the per-sample `Voice::process()` call in the engine. Engine mode, filter model, waveform, drive and sub level are template parameters, and state lives in locals across each 128-frame chunk. `process()` is now a 1-sample wrapper. The audio-path `SineLut::lookupFast()` drops the NaN test and `std::floor`.

**Equivalence:** an A/B harness renders 15 scenarios with the old and new library: every factory model, all 24 waveform × filter × drive × sub combinations, mono/poly, glide, 303 accent + slide, bend, mod-wheel vibrato, and the worker split. At `-O0` and `-O2` the output is **bit-identical** (max diff 0). At `-O3 -ffast-math`, max diff is 7e-4 (RMS ≤ 2e-5). The old code alone moves 1.7e-3 between `-O2` and `-O3 -ffast-math`, so that is compiler reassociation, not behaviour.

**Desktop micro-benchmark:** ns per voice per sample, from 8 held notes minus an idle engine, chorus off. Apple Silicon, clang, best of 5.

| Preset (path) | `-O2` before → after | `-O3 -ffast-math` before → after |
|---|---|---|
| Juno-60 (ladder, saw) | 31.0 → 26.7 | 28.4 → 27.2 |
| OB-Xa (ladder) | 31.2 → 26.6 | 28.6 → 26.4 |
| CS-80 (SVF) | 23.7 → 17.7 | 20.6 → 15.4 |
| DX E.Piano (FM) | 13.9 → 11.6 | 12.7 → 10.5 |

This shows direction only; it is not a prediction of ESP32-S3 speed (Xtensa GCC, no out-of-order core, 16 FP registers, flash cache). Measure on the board with the `mk4-synth-profile` build. Its `[BENCH]` line now reports `voiceblk` (block path) next to `voice` (the 1-sample wrapper, now slower per call, so it is no longer the engine's cost).
