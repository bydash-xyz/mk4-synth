# Third-party notices

mk4-synth's own code is MIT licensed (see [LICENSE](LICENSE)). The prebuilt firmware
(`mk4-synth.bin` on the [Releases](https://github.com/bydash-xyz/mk4-synth/releases) page) also
contains the following software, under its own license. The full license texts are in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), which is also attached to every release.

| Component | License | Source |
| :--- | :--- | :--- |
| Arduino core for ESP32 (incl. its BLE, Preferences, SPI and Wire libraries) | LGPL-2.1 (core); BLE library Apache-2.0 | https://github.com/espressif/arduino-esp32 |
| ESP-IDF and its bundled components (FreeRTOS, Bluetooth stack, drivers, ...) | Apache-2.0, with some components under their own permissive licenses | https://github.com/espressif/esp-idf |
| U8g2 | BSD-2-Clause | https://github.com/olikraus/u8g2 |
| BLE-MIDI | MIT | https://github.com/lathoub/Arduino-BLE-MIDI |
| Arduino MIDI Library | MIT | https://github.com/FortySevenEffects/arduino_midi_library |

The OLED glyph table in `lib/MpkMiniMk4Display/include/MpkOledFont.h` is pre-rendered from the
Arimo font (Copyright Google; Apache-2.0, later versions SIL OFL 1.1). No font file is included.

## Rebuilding with a modified library (LGPL-2.1)

The Arduino core is linked statically. To use a modified version of it, or of any other library
above, change the dependency in `platformio.ini` (or point PlatformIO at your own copy) and rebuild
with `pio run -e mk4-synth`. The complete source of mk4-synth and the exact toolchain and library
versions it builds with are in this repository.
