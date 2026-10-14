#!/usr/bin/env python3
"""Package the built mk4-synth firmware for release and for the web flasher.

Merges bootloader, partition table, boot_app0 and the app into one image that flashes at 0x0, and
writes an ESP Web Tools manifest and SHA256 sums next to it. Build the firmware first:

    pio run -e mk4-synth
    python3 tools/make_firmware_image.py --version v0.1.0      # -> build/release/

Uses `python3 -m esptool` when installed (pip install esptool), otherwise PlatformIO's esptool.
"""
import argparse
import glob
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.path.join(ROOT, ".pio", "build", "mk4-synth")
IMAGE = "mk4-synth.bin"

# Offsets from the Arduino default_16MB.csv layout the board uses (`pio run -t envdump`, FLASH_EXTRA_IMAGES).
# Header flash mode is DIO, as PlatformIO's upload writes it for this QIO board: the ROM loader can't
# read the bootloader in QIO at 80 MHz (boot loop at "ets_loader.c 78"); the bootloader switches to QIO itself.
FLASH_ARGS = ["--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB"]


def boot_app0():
    hits = glob.glob(os.path.expanduser(
        "~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin"))
    if not hits:
        sys.exit("boot_app0.bin not found: build the firmware with PlatformIO first")
    return hits[0]


def esptool():
    if importlib.util.find_spec("esptool"):
        return [sys.executable, "-m", "esptool"]
    return ["pio", "pkg", "exec", "-p", "tool-esptoolpy", "--", "esptool.py"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", help="version shown by the flasher (default: git describe)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release"), help="output directory")
    args = ap.parse_args(argv)

    version = args.version or subprocess.run(
        ["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT,
        capture_output=True, text=True, check=True).stdout.strip()
    for part in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        if not os.path.exists(os.path.join(BUILD, part)):
            sys.exit(f"{part} missing: run `pio run -e mk4-synth` first")

    os.makedirs(args.out, exist_ok=True)
    image = os.path.join(args.out, IMAGE)
    subprocess.run(esptool() + ["--chip", "esp32s3", "merge_bin", "-o", image] + FLASH_ARGS + [
        "0x0", os.path.join(BUILD, "bootloader.bin"),
        "0x8000", os.path.join(BUILD, "partitions.bin"),
        "0xe000", boot_app0(),
        "0x10000", os.path.join(BUILD, "firmware.bin"),
    ], check=True)

    # ESP Web Tools resolves `path` relative to the manifest URL, so the two files must sit side by side.
    manifest = {
        "name": "mk4-synth",
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": IMAGE, "offset": 0}]}],
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    with open(image, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    with open(os.path.join(args.out, "SHA256SUMS"), "w") as f:
        f.write(f"{digest}  {IMAGE}\n")
    print(f"{image} ({os.path.getsize(image)} bytes, {version})\nsha256 {digest}")


if __name__ == "__main__":
    main()
