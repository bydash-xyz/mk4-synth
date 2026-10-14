#!/usr/bin/env python3
"""Capture the ESP32-S3 USB serial log for N seconds (default 30) and print it.

Run it with PlatformIO's own Python, which ships pyserial (system python3 does not):

    ~/.local/pipx/venvs/platformio/bin/python tools/serial_capture.py 40 [port] [-o file] [-f substring]

DTR must be asserted or the USB CDC port stays silent, and `pio device monitor` cannot be scripted
(it needs an interactive terminal). Start the capture first, then play the instrument.
"""
import argparse
import glob
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _deps import require  # noqa: E402


def main(argv=None):
    parser = argparse.ArgumentParser(description="Capture the ESP32-S3 USB serial log for N seconds.")
    parser.add_argument("seconds", nargs="?", type=float, default=30)
    parser.add_argument("port", nargs="?", help="default: first /dev/cu.usbmodem*")
    parser.add_argument("-o", "--out", help="also save the raw log to this file")
    parser.add_argument("-f", "--filter", help="only print lines containing this text (e.g. PROFILE)")
    args = parser.parse_args(argv)

    require("serial")
    import serial

    port = args.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        sys.exit("no /dev/cu.usbmodem* port found; is the board connected?")

    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.dtr, ser.rts = True, False
    print(f"capturing {args.seconds:.0f}s from {port}", file=sys.stderr, flush=True)

    end = time.time() + args.seconds
    buf = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk

    text = buf.decode("utf8", "replace")
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    for line in text.splitlines():
        if not args.filter or args.filter in line:
            print(line)


if __name__ == "__main__":
    main()
