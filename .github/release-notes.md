Standalone polyphonic ESP32-S3 synthesizer firmware for the Akai MPK Mini MK4.

### Install

- **In the browser (easiest):** open **https://bydash.xyz/mk4-synth/** in Chrome or Edge, plug the board's **COM** port into your computer and click **Install**.
- **Command line:** `esptool.py --chip esp32s3 write_flash 0x0 mk4-synth.bin`

Updating keeps your saved user patches unless you choose to erase the device. Wiring and parts are in [HARDWARE.md](https://github.com/bydash-xyz/mk4-synth/blob/main/HARDWARE.md).

### Files

| File | What it is |
| :--- | :--- |
| `mk4-synth.bin` | Complete image (bootloader, partition table, app); flash at `0x0` |
| `manifest.json` | ESP Web Tools manifest used by the web flasher |
| `SHA256SUMS` | Checksum of `mk4-synth.bin` |
| `THIRD_PARTY_NOTICES.md` | Which libraries the firmware includes, and how to rebuild it with modified ones |
| `THIRD_PARTY_LICENSES.md` | Full license texts of those libraries |
