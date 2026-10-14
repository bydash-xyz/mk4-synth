#!/usr/bin/env python3
"""Save the MPK mini IV host framebuffer (what we draw over the Plugin Port) as an image.

Needs mido + python-rtmidi and the MK4 connected to this computer (not to the ESP32). Sends the 0x72 dump request
and decodes the three 0x71 reply chunks; the pixels come back byte-swapped.

    python3 tools/mpk_oled_capture.py screen.png      # PNG via macOS sips, x4 scale
    python3 tools/mpk_oled_capture.py screen.ppm      # plain PPM, 1:1
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _deps import require  # noqa: E402

require("mido", "rtmidi")
import mido  # noqa: E402

W, H = 160, 80


def find(names, key):
    for n in names:
        if key in n:
            return n
    sys.exit(f"No MIDI port containing {key!r}; is the MK4 connected to this computer?")


def capture():
    out = mido.open_output(find(mido.get_output_names(), "Plugin Port"))
    inp = mido.open_input(find(mido.get_input_names(), "Plugin Port"))
    for _ in inp.iter_pending():
        pass
    out.send(mido.Message("sysex", data=[0x47, 0x00, 0x5D, 0x72, 0x00, 0x00]))
    fb = [[(0, 0, 0)] * W for _ in range(H)]
    rows, deadline = 0, time.time() + 3
    while rows < H and time.time() < deadline:
        for m in inp.iter_pending():
            d = list(m.data) if m.type == "sysex" else []
            if len(d) < 14 or d[0] != 0x47 or d[3] not in (0x71, 0x72):
                continue
            p = d[6:]
            y0, h = (p[2] << 7) | p[3], (p[6] << 7) | p[7]
            px = p[8:]
            for i in range(min(len(px) // 3, W * h)):
                v = (px[3 * i] << 11) | (px[3 * i + 1] << 5) | px[3 * i + 2]
                v = ((v & 0xFF) << 8) | (v >> 8)
                fb[y0 + i // W][i % W] = ((v >> 11) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31)
            rows += h
        time.sleep(0.01)
    if rows < H:
        sys.exit(f"Only {rows}/{H} rows received")
    return fb


def main(argv=None):
    ap = argparse.ArgumentParser(description="Save the MPK mini IV host framebuffer as an image.")
    ap.add_argument("path", nargs="?", default="mpk_screen.png", help=".png (via macOS sips, x4) or .ppm (1:1)")
    path = ap.parse_args(argv).path
    fb = capture()
    ppm = path if path.endswith(".ppm") else path + ".ppm"
    with open(ppm, "wb") as f:
        f.write(b"P6 %d %d 255\n" % (W, H))
        f.write(bytes(c for row in fb for p in row for c in p))
    if ppm != path:
        subprocess.run(["sips", "-s", "format", "png", "-z", str(H * 4), str(W * 4), ppm, "--out", path],
                       check=True, capture_output=True)
        os.remove(ppm)
    print(path)


if __name__ == "__main__":
    main()
