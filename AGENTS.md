# AGENTS.md

Guidance for coding agents working on mk4-synth. For what the project does and how users build
and flash it, read [README.md](README.md) first; [CONTEXT.md](CONTEXT.md) has the domain terms
and [docs/adr/](docs/adr/) the design decisions. This file covers what an agent needs on top of
that.

## Hard rules

- **Never flash the board on your own.** `pio run -t upload`, `make flash`, `make profile` and
  `make dac-test` write to the ESP32-S3. Only run them when the user asks in so many words.
  Building (`pio run -e mk4-synth`) is always fine.
- **Never commit Akai material.** No firmware images (`*.bin`, `firmware/`), Studio Instrument
  Collection files (`*.big`, plugin binaries), disassembly output, or text and images copied from
  Akai software or manuals. They are git-ignored; keep it that way. Protocol facts learned by
  observing the MK4 are fine, written in your own words (see `MpkMiniMk4Display.h` and the ADRs).
- **The MPK Mini MK4 is the only supported controller.** Don't assume MK3 hardware, layout or
  SysEx; they differ. `docs/MPK_MINI_IV_MIDI_REFERENCE.md` is the reference.
- **Issues and PRs need a human-written summary.** CONTRIBUTING.md requires every issue and pull
  request to start with at least one sentence written by a person. When you draft one, leave the
  "Summary (written by you)" field for the user and ask them for it. Never write it yourself, and
  don't paraphrase their words into it.
- **`.scratch/` is local-only.** It holds the maintainer's private notes (reverse-engineering,
  old reviews, specs) and is git-ignored. You may read it for background. Never commit it, and
  never link to it from tracked files.

## Commands

`make help` lists the shortcuts. The raw commands:

```bash
cmake -B build -S . && cmake --build build -j       # desktop build: engine, tests, sim, tools
ctest --test-dir build -LE hardware --output-on-failure
ctest --test-dir build -L hardware                   # needs a real MK4 on USB; only when asked
./build/synth_render --preset all --check            # every factory preset renders cleanly
./build/synth_render --preset 3 --out build/p3.wav   # listen to a change
make manual                                          # regenerate docs/mpk_mini_mk4_manual.html
pio run -e mk4-synth                              # firmware (primary target)
pio run -e mk4-synth-profile                      # firmware with timing probes
pip install -r tools/requirements.txt                # Python tools; python3 tools/synthctl.py --help
```

If test results contradict a header you just edited, rebuild with `--clean-first` before
debugging.

## How to verify a change

1. The desktop build has no new warnings and `ctest -LE hardware` passes.
2. `pio run -e mk4-synth` and `pio run -e mk4-synth-profile` build.
3. If you touched `Presets.h` or knob mappings: `make manual` and commit the regenerated manual
   (`ManualSyncTest` fails otherwise). `PresetLoudnessTest` must still pass; don't loosen its
   limits to make a preset fit.
4. If you touched DSP code: `DspFidelityTest` passes, and render the affected presets to WAV and
   say what you listened for. [docs/dsp-fidelity-review.md](docs/dsp-fidelity-review.md) lists
   the issues those tests pin.
5. If you touched SysEx or NVS: `SysExProtocolTest` passes, including the v1 NVS migration.

Desktop tests can't exercise USB host, BLE or I2S timing. Only the user can confirm on the board;
say so when you report, rather than claiming it works on hardware.

## Invariants the firmware relies on

These were learned the hard way; code must keep them:

- **Audio budget:** 2902 us per 128-frame block. Anything over is audible static. Measure with the
  profiling build (below) before and after any change to the voice or effect code.
- **The audio task never locks.** Note events and parameter pushes go through the deferred event
  queue (`SynthEngine::setDeferredEvents(true)`); don't add mutexes, critical sections or heap
  allocation on the audio path.
- **No per-event logging in the USB host task.** A blocking `Serial` print per MIDI event makes
  the MK4 drop events, which shows up as stuck notes. Log only rare events (enumeration, SysEx).
- **Keep `-O3` effective.** Arduino appends `-Os`; `mk4-synth` unflags it in `platformio.ini`.
  Check with `pio run -t compiledb` if speed regresses.
- **Patch format (ADR-0006):** dumps are parameter lists. Readers skip unknown IDs and default
  missing ones; never reuse or renumber an ID. Factory slots are read-only; only user slots
  (`USER_SLOT_START..USER_SLOT_END`, 24..32) are written over SysEx and stored in NVS.
- **SysEx identity:** manufacturer `0x7D` (non-commercial), family `0x01`. Don't change them;
  saved patches and the patch editor depend on them.
- **BLE is for patches only.** Performance MIDI comes from the MK4 over USB host.
- `tools/patch_ble_midi.py` patches BLE-MIDI 2.2 for arduino-esp32 3.x on every build; don't edit
  `.pio/libdeps` by hand.

## Profiling on the board

Profiling is compiled in only when `SYNTH_PROFILE` is defined (`src/profiling.h`). Only when the
user asks you to flash:

```bash
pio run -e mk4-synth-profile -t upload
# Start the capture first, then play. PlatformIO's Python has pyserial and handles DTR.
~/.local/pipx/venvs/platformio/bin/python tools/serial_capture.py 40 -f PROFILE
pio run -e mk4-synth -t upload                    # back to the normal firmware
```

- `over_budget=N/344` counts blocks that exceeded the audio budget.
- Don't use `pio device monitor` (needs an interactive terminal), `cat /dev/cu.usbmodem*` (USB
  CDC needs DTR) or `timeout` (not on macOS).
- Add probes as `PROFILE_*` macros in `src/profiling.h`, not ad-hoc prints.
- Voices are split between Core 1 (audio task and `loop()`) and `Core0VoiceWorker`
  (`src/esp32s3_driver.h`) whenever 3 or more are sounding.

## Layout

| Path | Contents |
| :--- | :--- |
| `lib/SynthEngine/` | Platform-independent DSP engine, presets, MIDI parser, SysEx protocol |
| `lib/MpkMiniMk4Display/` | MK4 control surface and screen SysEx |
| `src/` | ESP32-S3 firmware: I2S driver, USB host MIDI, OLED UI, BLE patch server |
| `sim/` | Desktop simulator and offline renderer |
| `tests/` | Host unit tests; `mock_hal/` stands in for Arduino and ESP-IDF |
| `tools/` | Python and C++ helpers; generated files say which tool made them |
| `diagnostics/` | Standalone bring-up sketches per hardware part |
| `docs/` | ADRs, MK4 MIDI reference, generated manual, DSP review |

## Style

Match the surrounding code: C++20, engine code free of Arduino headers so it builds on the
desktop, hardware access behind the drivers in `src/` so tests can mock it, and comments that
explain *why* (usually a measured hardware or timing quirk). Generated files (`MpkOledFont.h`,
the manual) are changed through their generator, not by hand. Record a new design decision as
an ADR in `docs/adr/`.
