# mk4-synth

A standalone polyphonic ESP32-S3 synthesizer for the Akai MPK Mini MK4.

It runs on an ESP32-S3 and on the desktop. The same C++ `SynthEngine` provides the factory sounds (Juno-60, Minimoog, Prophet-5, TB-303, CS-80, OB-Xa, Casio PT-10, 2-operator FM keyboards, PPG-style wavetable, Casio CZ phase distortion, Karplus-Strong plucked strings and a Hammond drawbar organ) and user patch slots. On hardware it is played from an Akai MPK Mini MK4 plugged into the ESP32-S3's USB host port, shows its state on a small OLED, and outputs audio through a PCM5102A I2S DAC. Wireless patch editing uses BLE SysEx. On the desktop, `synth_sim` plays the same engine through PortAudio, and `synth_render` renders presets offline to WAV.

Install the firmware from the browser at [bydash.xyz/mk4-synth](https://bydash.xyz/mk4-synth/), and play the OLED screen animations there before you build anything: [bydash.xyz/mk4-synth/animations](https://bydash.xyz/mk4-synth/animations/).

Preset layout (32 slots, see [CONTEXT.md](CONTEXT.md) for the full list and domain terms):

| Slots | Contents |
| :--- | :--- |
| 1-6 | Subtractive analog: Juno-60, Minimoog, Prophet-5, TB-303, CS-80, OB-Xa |
| 7-10 | Casio PT-10 (Piano, Fantasy, Violin, Flute) |
| 11-15 | 2-op FM (DX7 e-piano, Crystal Tines, Rhodes, Clavinet, Organ) |
| 16-17 | Wavetable (PPG Wave, Waldorf-style vocal pad) |
| 18-19 | Casio CZ phase distortion (bass, resonance lead) |
| 20-21 | Karplus-Strong plucked string (guitar, koto) |
| 22-23 | Additive Hammond drawbar organ (jazz, gospel) |
| 24-32 | `User_01`..`User_09`, patches written over SysEx and stored in NVS |

Each preset has 16 parameters: Bank A is knobs 1-8, Bank B is knobs 9-16.

## Hardware at a glance

| Part | Model | Buy |
| :--- | :--- | :--- |
| MCU | ESP32-S3 N16R8 dual USB-C board (16 MB flash, 8 MB octal PSRAM) | [Amazon](https://amzn.to/47Ypoyo) |
| DAC | PCM5102A (GY-PCM5102 breakout) on I2S, GPIO 4/5/6 | [Amazon](https://link.amazon/B07QhN2h2) |
| Display | 0.96" SSD1306 OLED, I2C, GPIO 8 (SDA) / 9 (SCL) | [Amazon](https://link.amazon/B0cLaqKIj) |
| Controller | Akai MPK Mini MK4 on the native USB host port (GPIO 19/20) | [Amazon](https://link.amazon/B08fwMN1S) |

The Amazon links are affiliate links: buying through them supports the project at no extra cost to you.

The build guide, with the exact parts, tools, a wiring diagram, bring-up steps and troubleshooting, is [HARDWARE.md](HARDWARE.md). Bring-up sketches for each part are in [diagnostics/](diagnostics/README.md).

## Flash the firmware

You don't need to install anything: the web flasher writes the latest release from the browser.

1. Build the hardware ([HARDWARE.md](HARDWARE.md)), including bridging the two solder pads under the ESP32-S3 board.
2. Unplug the MK4 from the board. Connect the board's **COM** port to your computer with a USB-C data cable. Never connect both of the board's USB-C ports to a computer at once.
3. Open **[bydash.xyz/mk4-synth](https://bydash.xyz/mk4-synth/)** in Chrome or Edge on a desktop computer (Safari and Firefox can't talk to serial ports).
4. Click **Install**, pick the board's serial port (`usbmodem…` on a Mac, `COM…` on Windows, often shown as "USB Single Serial") and confirm.
5. Erase the device only on the first install. When updating, leave erase off: it would delete your saved user patches.
6. When it finishes (about a minute), the board restarts. Plug the MK4 into the board's **USB** port and play.

If the port doesn't appear, try another cable (charge-only cables have no data lines); on Windows, install the WCH CH343 serial driver. If the flasher can't connect, hold **BOOT**, tap **RST**, release **BOOT** and try again.

Prefer the command line? Download `mk4-synth.bin` from [Releases](https://github.com/bydash-xyz/mk4-synth/releases) and run `esptool.py --chip esp32s3 write_flash 0x0 mk4-synth.bin`. To build it yourself, see the quick start below.

## Quick start

Everything below is a shortcut in the `Makefile`; raw commands are listed after the table. Run `make help` for the current list.

Prerequisites:

```bash
brew install cmake portaudio rtmidi     # desktop build (without the last two the simulator is skipped)
pipx install platformio                 # firmware build; or pip install platformio
pip install -r tools/requirements.txt   # Python tools (pyserial, mido, python-rtmidi, pillow)
```

| Command | What it does |
| :--- | :--- |
| `make help` | List all targets |
| `make build` | Configure and build all desktop targets into `build/` |
| `make test` | Run the unit tests (`ctest -LE hardware`) |
| `make test-hw` | Run the tests that need a physical MPK Mini MK4 attached |
| `make sim ARGS="--preset 3"` | Run the desktop simulator |
| `make render ARGS="--preset 2 --out build/moog.wav"` | Render a preset offline; with no `ARGS` it renders preset 1 to `build/render.wav` |
| `make manual` / `make manual-pdf` | Regenerate the MK4 manual (`docs/mpk_mini_mk4_manual.html`); `manual-pdf` also writes `build/mpk_mini_mk4_manual.pdf` for printing |
| `make firmware` | Build the ESP32-S3 firmware (`mk4-synth`) |
| `make firmware-image` | Build and package `build/release/mk4-synth.bin` with the flasher manifest, as a release does |
| `make flash` | Build and upload the firmware |
| `make profile` | Build and upload the profiling firmware (`mk4-synth-profile`) |
| `make capture SECS=40 FILTER=PROFILE` | Capture the board's USB serial log for 40 s, keeping lines that contain FILTER |
| `make dac-test` | Build and upload the PCM5102A 440 Hz test tone |
| `make clean` | Remove build outputs |

Without make:

```bash
cmake -B build -S .
cmake --build build -j
ctest --test-dir build -LE hardware --output-on-failure     # unit tests
ctest --test-dir build -L hardware --output-on-failure      # needs the MPK attached
./build/synth_sim [options]
./build/synth_render --preset 1 --out build/render.wav

pio run -e mk4-synth                # firmware (default env)
pio run -e mk4-synth -t upload      # flash
pio run -e mk4-synth-profile -t upload
pio run -e diag_dac_test -t upload
~/.local/pipx/venvs/platformio/bin/python tools/serial_capture.py 40
```

Test executables (each is also a CTest test): `synth_test` (core DSP), `dsp_fidelity_test`, `sysex_test` (SysEx and NVS persistence), `esp32s3_driver_test`, `hardware_driver_test` (OLED, USB MPK, BLE SysEx against mock HAL), `mpk_control_surface_test`, `sim_tools_test` (simulator keyboard map and WAV/MIDI helpers), plus `SynthRenderSmokeTest` (`synth_render --preset all --check`), `PresetLoudnessTest` (every factory preset plays a chords-and-melody phrase within 1.5 LU of the others) and `ManualSyncTest`. `MpkHardwareVerifyTest` is labelled `hardware` and needs the controller.

The ESP32 flash uses the board's UART port; the MPK goes on the second (USB OTG) port.

## Desktop simulator

`synth_sim` routes audio through PortAudio and listens to an MPK Mini MK4 (auto-detected), the `SynthEngine Virtual In` MIDI port, and, when stdin is a terminal, the computer keyboard.

Flags:

| Flag | Meaning |
| :--- | :--- |
| `--preset N` | Start on slot N (1..32, default 1) |
| `--audio-device D` | Output device by index or name substring |
| `--midi-port S` | Also listen on MIDI inputs whose name contains S |
| `--buffer FRAMES` | Audio buffer, 16..4096 (default 128) |
| `--list-devices` | List audio outputs and MIDI inputs, then exit |
| `--no-keyboard` | Disable computer-keyboard play mode |
| `--test` | Headless smoke test, then exit |

### Keyboard play mode

Press `?` in the simulator to print this table.

| Keys | Action |
| :--- | :--- |
| `a s d f g h j k l ;` | White keys C D E F G A B C D E |
| `w e` `t y u` `o p` | Black keys C# D# / F# G# A# / C# D# |
| `z` / `x` | Octave down / up (`a` = C4 by default) |
| `1`..`9` | Velocity 14..127 (default 100) |
| `,` `.` or `[` `]` | Previous / next preset |
| `b` | Knob bank A / B |
| `m` | Toggle GATE / HOLD note mode |
| `-` / `=` | Gate time -/+ 100 ms (100..3000 ms, default 400) |
| Space | Panic (all sound off) |
| `?` / `q` | Help / quit (Ctrl+C also quits) |

A terminal sends key presses but no key releases, so note-off is synthesized:

- GATE (default): each note is released after the gate time. Holding a key (autorepeat) keeps it sounding.
- HOLD: a note sounds until you press its key again. Tap keys one at a time to build chords or drones. Switching mode releases everything.

### Offline rendering

`synth_render` writes 44.1 kHz stereo WAV files from the same engine, which is useful for checking a sound without audio hardware.

```bash
./build/synth_render --preset 2 --out build/moog.wav
./build/synth_render --preset 4 --notes "A2:0.25 r:0.25 A2:0.25 C3:0.5" --out build/acid.wav
./build/synth_render --preset 1 --midi song.mid --out build/song.wav
./build/synth_render --preset all --out build/all.wav       # writes all_01.wav .. all_15.wav
./build/synth_render --preset all --check                   # verify only, no file needed
./build/synth_render --preset all --notes "C3,E3,G3 A3 C4" --velocity 110 --max-spread 2   # compare preset loudness (LUFS)
```

Note spec (`--notes`, default `C4,E4,G4`): steps separated by spaces, chord notes joined by commas, `r` for a rest, `:SEC` to override a step's length (for example `C3 E3 G3 C4,E4,G4:1.5`). Other options: `--dur SEC` (step length, default 2.0), `--velocity V` (1..127, default 100), `--midi FILE` (play a Standard MIDI File instead of `--notes`), `--tail SEC` (release time after the last event, default 1.0), `--float` (32-bit float samples).

`--check` exits 1 on NaN/inf, samples beyond full scale, or silence. CTest runs it over every factory preset as `SynthRenderSmokeTest`.

## Using it with the MPK Mini MK4

- Plug the MK4 into the ESP32-S3's USB OTG port. The controller is the only supported input; performance MIDI comes over USB host only (BLE is for patch updates).
- Knobs K1-K8 edit Bank A; the BANK A/B button switches to Bank B (knobs 9-16). Shift + knob is the MK4's own fine adjust and does not change bank. K8 (CC 7) is also master volume.
- Preset navigation: turn the Nav dial, or press the left / right cursor keys (`<` / `>`). Shift + Nav dial steps presets by 4. Shift + a keybed note selects the root key.
- The MK4 must be in the mode that exposes its Plugin Port. The firmware seeds the knob values and draws the knob labels on the MK4's screen over Plugin Port SysEx; without it parameter updates halt and the synth warns you (PORT OFF below). See [CONTEXT.md](CONTEXT.md) (MpkMiniMk4ControlSurface) and [ADR 0005](docs/adr/0005-mpk-mini-mk4-control-surface-and-knob-pre-seeding.md).
- To send a whole patch from a computer, pair `MK4-Synth` over Bluetooth and open [`tools/patch_sender.html`](tools/patch_sender.html) in Chrome or Edge. TRY ON SYNTH loads the patch without saving it; SAVE TO SLOT stores it in a user slot (24-32).

### OLED fault indicators

Serial is usually not attached, so faults are shown on the SSD1306 (definitions in `src/system_status.h`, drawing in `src/oled_ui.h`).

| On screen | Kind | Meaning and what to check |
| :--- | :--- | :--- |
| `NO AUDIO` page, "I2S DAC init failed / check PCM5102A wiring" | Fatal, full screen | The I2S channel for the DAC could not start. Check BCK/LRCK/DIN wiring and the DAC's SCK-to-GND strap. |
| `NO AUDIO` page, "audio task failed / low memory - reboot" | Fatal, full screen | The audio task could not be created (out of internal RAM). Reboot. |
| `NO MPK` tag (banner "MPK Mini MK4 unplugged") | Warning | The MK4 is not on USB host (not found within 5 s of boot, or unplugged). |
| `PORT OFF` tag (banner "MPK Plugin Port offline") | Warning | The MK4 enumerated but sent no Akai SysEx reply for 3 s. Knob seeding and the MK4 screen are off. Check the Plugin Port requirement above. |
| `NO NVS` tag (banner "Patches won't be saved") | Warning | NVS could not open; user patches are not saved or restored. |
| `NO BLE` tag (banner "BLE patch link off") | Warning | The BLE controller did not start; no wireless patch updates. |
| `NO PSRAM` tag (banner "PSRAM not detected") | Warning | PSRAM was not detected (the N16R8 should have 8 MB). The synth itself only uses internal RAM. |
| "Connect MPK Mini MK4 / to the USB host port" | Waiting page | Shown from boot until the MK4 first enumerates. After the 5 s timeout the last line becomes "not found - check cable". |

Warnings appear as a tag in the header plus a 3-second banner when raised; they clear themselves when the condition goes away. Fatal faults take over the whole screen. Every fault is also logged on serial as `[Fault] ...`.

## Profiling and serial debugging

Profiling code is compiled in only when `SYNTH_PROFILE` is defined (`src/profiling.h`); the normal firmware contains none of it.

```bash
make profile                 # flash the profiling build (boot benchmark, audio block timing, OLED frame timing)
make capture SECS=40         # start this FIRST, then play
make flash                   # back to the normal firmware when done
```

- The audio budget is 2902 us per 128-frame block. `over_budget=N/344` counts blocks that exceeded it (audible static).
- Capture with PlatformIO's Python (it has pyserial and the required DTR handling): `~/.local/pipx/venvs/platformio/bin/python tools/serial_capture.py 40 -f PROFILE`.
- Do not use `pio device monitor` (needs an interactive terminal), `cat /dev/cu.usbmodem*` (USB CDC needs DTR) or `timeout` (not on macOS).
- Add new probes as `PROFILE_*` macros in `src/profiling.h`, not by editing firmware files.
- Firmware builds must keep `-O3` effective: Arduino adds `-Os` last, so `mk4-synth` unflags it. Check with `pio run -t compiledb` if speed regresses.
- Voices are split between Core 1 (audio task and `loop()`) and a worker on Core 0 (`Core0VoiceWorker` in `src/esp32s3_driver.h`) when 3 or more are sounding.
- `tools/patch_ble_midi.py` patches BLE-MIDI 2.2 for arduino-esp32 3.x on every build; do not edit `.pio/libdeps` by hand.

## Python tools

`tools/synthctl.py <command> [args]` is the single entry point (install `tools/requirements.txt` first; each command's `--help` lists its options):

| Command | Script | Purpose |
| :--- | :--- | :--- |
| `capture` | `serial_capture.py` | Capture the ESP32-S3 USB serial log for N seconds |
| `midi` | `midi_cli.py` | Send and monitor MIDI, SysEx params and CCs for board testing |
| `oled-capture` | `mpk_oled_capture.py` | Save the MPK Mini MK4 screen framebuffer as PNG/PPM |
| `gen-font` | `gen_oled_font.py` | Generate the MK4 OLED glyph table header (`MpkOledFont.h`) |
| `gen-manual` | `generate_manual.py` | Generate or verify the MPK Mini MK4 HTML manual; `--pdf` also prints a US Letter PDF to `build/` with headless Chrome |

Other files in `tools/`: `patch_ble_midi.py` (PlatformIO pre-build script), `manual_generator.cpp` and `mpk_display.cpp` (C++ helpers built by CMake), `_deps.py` (shared dependency check).

## Repo map

| Path | Contents |
| :--- | :--- |
| `lib/SynthEngine/` | Platform-independent DSP engine, presets, MIDI parser, SysEx protocol |
| `lib/MpkMiniMk4Display/` | MPK Mini MK4 control surface and screen SysEx |
| `src/` | ESP32-S3 firmware: `main.cpp`, I2S driver, USB host MIDI, OLED UI and `animations/`, BLE server, `system_status.h`, `profiling.h` |
| `sim/` | Desktop simulator (`synth_sim`) and offline renderer (`synth_render`) |
| `tests/` | Unit tests, with `mock_hal/` replacing Arduino/ESP32 peripherals on the host |
| `tools/` | Python and C++ helper tools (see above) |
| `diagnostics/` | Standalone bring-up sketches for each hardware part |
| `docs/` | `adr/` (architecture decision records), DSP fidelity review, MK4 MIDI reference and generated manual, wiring diagram |
| `platformio.ini` | Firmware environments: `mk4-synth`, `mk4-synth-profile`, `diag_dac_test` |
| `CMakeLists.txt` | Desktop build, tests and tools |
| `.github/` | CI workflow (see below), issue and pull request templates |
| `AGENTS.md`, `CLAUDE.md` | Rules, commands and invariants for coding agents |
| `CONTRIBUTING.md` | How to report problems, add presets and send pull requests |
| `THIRD_PARTY_NOTICES.md` | Licenses of the libraries in the prebuilt firmware |
| `CONTEXT.md` | Domain glossary and architecture overview |
| `HARDWARE.md` | Build guide: parts, wiring diagram, bring-up and troubleshooting |

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on pushes to `main` and on pull requests:

- `desktop-tests` (macOS): installs `portaudio` and `rtmidi`, configures and builds with CMake, checks that `synth_sim` and `mpk_display` were built, then runs `ctest -LE hardware`.
- `firmware` (Ubuntu): installs PlatformIO and builds `mk4-synth` and `mk4-synth-profile`.

## Contributing

Bug reports, sound fixes, new presets and reports from builds on other hardware are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: every issue and pull request needs a summary written by a person.

## License

MIT, see [LICENSE](LICENSE). The generated OLED glyph table (`lib/MpkMiniMk4Display/include/MpkOledFont.h`) is rendered from the Arimo font, Copyright Google, Apache License 2.0.

## Disclaimer

This is an independent hobby project and is not affiliated with or endorsed by Akai Professional or inMusic. The MPK Mini MK4 screen and preset SysEx support was worked out for interoperability; no Akai firmware or software is included. Juno-60, Minimoog, Prophet-5, TB-303, CS-80, OB-Xa, DX7, PPG Wave, CZ, Hammond and the other instrument names are trademarks of their respective owners and are used only to describe the sounds the presets approximate.
