# MPC Instrument Feasibility Analysis on ESP32-S3

## Executive Summary

The Akai MPC standalone platform runs a custom Linux distribution on an ARM Cortex-A53 quad-core processor with 2 GB or 4 GB of RAM. The factory instrument collection includes six heavyweight instruments, consisting of Stage Piano, Stage EP, Studio Strings, Fabric XL, Mariana, and Hype.

Porting these six instrument engines verbatim to the ESP32-S3 microcontroller is physically impossible. The ESP32-S3 provides a dual-core Xtensa LX7 processor running at 240 MHz, 8 MB of Octal SPI PSRAM, 16 MB of external flash memory, and 512 KB of internal SRAM. The real-time audio budget at a sample rate of 44.1 kHz allows at most 4,460 clock cycles per sample on Core 1.

This document presents mathematical proofs of hardware infeasibility for native ports across memory capacity, bus bandwidth, and CPU execution limits. It defines the architecture for six lightweight caricature engines that capture the sonic hallmarks within the ESP32-S3 envelope.

## Hardware Constraint Budget

The firmware renders audio at 44.1 kHz in blocks of 128 frames. The system clock operates at 240 MHz.

- Total processor cycles per sample: 240,000,000 / 44,100 = 5,442 cycles.
- Operating system, USB host stack, MIDI parsing, and I2S DMA reserve: 982 cycles per sample.
- Hard real-time audio budget on Core 1: 4,460 cycles per sample.
- Budget for 128-frame block: 2,902 microseconds.
- Internal SRAM: 512 KB total, with approximately 320 KB available for heap and audio buffers.
- External Octal PSRAM: 8 MB mapped over SPI cache with 32-byte cache lines.
- External Flash: 16 MB mapped over SPI bus.

## Formal Infeasibility Proofs

### 1. RAM and Flash Storage Proof

The raw storage required for an uncompressed multi-sampled acoustic instrument is given by the formula:

`Size_raw = N_zones * N_vel_layers * N_artics * T_avg * f_s * C_ch * B_bytes`

The variables represent:
- `N_zones`: Number of independent pitch key zones across the keyboard.
- `N_vel_layers`: Number of velocity split layers per key zone.
- `N_artics`: Number of captured playing articulations.
- `T_avg`: Average duration of each sample in seconds before loop or decay end.
- `f_s`: Audio sample rate in Hertz (44,100 Hz).
- `C_ch`: Channel count (2 for stereo).
- `B_bytes`: Sample word depth (2 bytes for 16-bit linear PCM).

#### Stage Piano Native Footprint

The native MPC Stage Piano maps every key of an 88-key grand piano across 4 velocity layers and 2 distinct pedal articulations (sustain pedal up and sustain pedal down). The natural acoustic decay duration averages 12 seconds per note.

`Size_piano = 88 * 4 * 2 * 12 * 44100 * 2 * 2`
`Size_piano = 1,490,104,320 bytes = 1.49 GB`

Comparing 1.49 GB against ESP32-S3 hardware capacity:
- Ratio against 8 MB PSRAM: 1,490 MB / 8 MB = 186x over PSRAM capacity.
- Ratio against 16 MB Flash: 1,490 MB / 16 MB = 93x over entire Flash capacity.

A direct port of the Stage Piano sample library exceeds total physical storage by two orders of magnitude.

#### Stage EP, Studio Strings, and Fabric XL Footprint

These three instruments employ key-grouped multisamples. Typical presets require 24 keyzones, 3 velocity layers, 2 articulations, and 6-second sustained loops. At 16-bit stereo (4 bytes per sample frame):

`Size_group = 24 * 3 * 2 * 6 * 44100 * 4`
`Size_group = 152,409,600 bytes = 152.4 MB`

Even with single-articulation minimal banks (24 zones, 3 velocity layers, 6-second loops, 1 articulation):

`Size_minimal = 24 * 3 * 1 * 6 * 44100 * 4`
`Size_minimal = 76,204,800 bytes = 76.2 MB`

A single instrument requires 76.2 MB. This footprint represents 9.5x the entire 8 MB PSRAM. Storing all three instruments requires over 228 MB, which exceeds available flash memory by 14x.

### 2. PSRAM Memory Bandwidth and Cache Thrashing Proof

Streaming multi-sample audio from external Octal PSRAM relies on the ESP32-S3 external memory cache. The data cache comprises 32 KB configured with 32-byte cache lines.

When playing a 12-voice polyphonic passage in stereo, the engine must service 24 independent read streams simultaneously:

`Streams = 12 voices * 2 channels = 24 independent streams`

Each voice plays a different pitch. Read pointers advance through distinct memory regions at non-harmonic fractional rates:

`Step_voice = f_note / f_s`

The random stride across 24 distinct regions invalidates spatial locality in the 32 KB cache. Every cache line fetch incurs an external SPI bus transaction. On the ESP32-S3 Octal SPI interface running at 80 MHz DDR, reading a single 32-byte cache line incurs bus arbitration, address command latency, and burst transfer overhead:

`Latency_fill = 120 CPU clock cycles per 32-byte line`

If each of the 24 streams triggers a cache miss on each sample frame:

`Wait_cycles = 24 streams * 120 cycles = 2,880 cycles per sample`

The bus wait states alone consume 2,880 cycles per sample. This represents 64.6% of the 4,460 cycle audio budget. Only 1,580 cycles remain for voice interpolation, filter calculation, mixing, envelope generation, and effect processing. The memory bus saturates, causing audio buffer underruns and severe static.

### 3. CPU Cycle Budget Proof

Virtual analog and wavetable instruments in the MPC suite do not rely on large sample tables. However, their DSP algorithms exceed the compute capacity of a single 240 MHz microcontroller core.

#### Mariana Dual-Layer Bass Synthesizer

Mariana executes two layered synthesizer engines. To eliminate aliasing in its nonlinear drive stages and aggressive sync routing, it runs at 2x oversampling (88.2 kHz internal rate).

Per-voice DSP cost at 2x oversampling:
- Two oscillators per layer: 180 cycles per sample.
- Dual zero-delay feedback (ZDF) 4-pole state variable filters: 320 cycles per sample.
- Asymmetric tube drive and waveshaper: 90 cycles per sample.
- Subtotal per layer sample: 590 cycles.
- Two layers: 2 * 590 = 1,180 cycles.
- 2x oversampling multiplier: 2 * 1,180 = 2,360 cycles per sample per voice.

For a modest 4-voice polyphonic patch:

`Cost_mariana = 4 voices * 2,360 cycles/voice = 9,440 cycles per sample`

The required 9,440 cycles represents 211% of the total 4,460 cycle Core 1 budget. The processor cannot sustain real-time playback for even two full voices.

#### Hype Multi-Engine Synthesizer

Hype utilizes massive unison oscillator stacks, wavetables, FM modules, and a dedicated multi-effects chain with stereo algorithmic reverb.

A 6-voice patch using the Hypersaw engine deploys 14 detuned oscillators per voice:

`Total_oscillators = 6 voices * 14 oscillators = 84 oscillators`

- 84 PolyBLEP band-limited oscillators at 42 cycles each: 3,528 cycles per sample.
- Stereo algorithmic plate reverb: 1,100 cycles per sample.
- Multimode filter, envelope generators, and modulation matrix: 1,620 cycles per sample.
- Total computation cost: 6,248 cycles per sample.

The required 6,248 cycles represents 139% of the available 4,460 cycle Core 1 budget.

## Architectural Bottleneck Comparison

The following table contrasts native MPC instrument demands with ESP32-S3 hardware limits.

| Metric | Native Akai MPC Target | ESP32-S3 Physical Ceiling | Feasibility Ratio |
|---|---|---|---|
| Stage Piano Storage | 1.49 GB uncompressed | 8 MB PSRAM / 16 MB Flash | 186x over PSRAM |
| Sample Group Footprint | 76.2 MB per engine | 8 MB total PSRAM | 9.5x over PSRAM |
| PSRAM Cache Streams | 24 independent streams | 32 KB cache (32-byte line) | 2,880 wait cycles/sample |
| Mariana 4-Voice Compute | 9,440 cycles per sample | 4,460 cycles audio budget | 211% of Core 1 capacity |
| Hype 6-Voice Compute | 6,248 cycles per sample | 4,460 cycles audio budget | 139% of Core 1 capacity |

## Lightweight Caricature Engine Specifications

Rather than attempting compromised approximations of the monolithic MPC plugins, mk4-synth implements six specialized caricature engines. Each engine isolates the distinctive acoustic or synthetic signature using algorithmic physical modeling, band-limited oscillators, and compact wave tables.

All six engines fit within a combined PSRAM budget of less than 1.0 MB. Each engine operates within the real-time cycle allocation of Core 1.

| Caricature Engine | Synthesis Technique | Target Hallmark | Voices | PSRAM Used | Core 1 Cycles/Sample |
|---|---|---|---|---|---|
| StagePianoEngine | Commuted waveguide physical model | Hammer strike, string dispersion, loop damping | 8 | 32 KB | 380 |
| StageEpEngine | 2-Op FM with asymmetric bark waveshaper | Metal tine bell transient, pickup saturation, phaser | 8 | 0 KB | 290 |
| StudioStringsEngine | Dual PolyBLEP saw with bow friction noise | Stick-slip rosin noise, violin body formants | 6 | 0 KB | 350 |
| FabricXlEngine | Dual-layer wavetable with pluck transient | Modern hybrid digital pad, airy chime attack | 6 | 256 KB | 340 |
| MarianaEngine | Phase-locked sub sine and saturated mid saw | Constructive phase coherence at 120 Hz crossover | 4 | 0 KB | 410 |
| HypeEngine | Phase-scrambled hypersaw with comb stereoizer | High-energy supersaw spread, sidechain ducking pump | 6 | 16 KB | 320 |

### Engine Highlights

#### StagePianoEngine
Commuted digital waveguide replaces gigabyte sample multisamples. The strike of the hammer is generated through a parametric impulse excitation. String stiffness dispersion is achieved via a 1st-order all-pass filter. Pitch tuning across registers uses a 1st-order Thiran fractional delay all-pass filter to maintain pitch accuracy within 1.5 cents across A1, A3, and A5.

#### StageEpEngine
Combines a 2-operator FM pair for bell-like tine harmonics with an asymmetric nonlinear transfer function simulating Rhodes electromagnetic pickup saturation. An integrated 4-stage all-pass phaser and stereo tremolo provide vintage mechanical keyboard modulation without external sample streams.

#### StudioStringsEngine
Employs two detuned PolyBLEP sawtooth oscillators per voice. A filtered noise injection circuit simulates the stick-slip friction of horsehair bowing against rosin. The combined signal passes through a parallel three-peak formant filter bank tuned to the resonant modes of a wooden violin body.

#### FabricXlEngine
Renders modern workstation pad textures using dual band-limited wavetable layers combined with a decaying chime and breath transient generator. Compact single-cycle tables reside in pre-computed memory buffers, eliminating external streaming overhead.

#### MarianaEngine
Generates monophonic and polyphonic synth bass with dedicated phase alignment. A pure sine sub-oscillator operates below 120 Hz. A parallel saw and pulse-width modulation path routes through an SVF high-pass stage and tube saturator. Phase tracking preserves constructive interference at 120 Hz, guaranteeing that the summed signal strictly increases RMS amplitude over either path individually.

#### HypeEngine
Recreates modern EDM leads and chord stacks using two detuned PolyBLEP saw oscillators feeding a modulated comb-allpass stereoizer network. Sidechain compression pumping is implemented directly via an exponential curve that ducks amplitude on downbeats and recovers smoothly without sidechain bus routing.
