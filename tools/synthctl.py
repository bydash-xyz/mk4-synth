#!/usr/bin/env python3
"""Single entry point for the helper scripts in tools/.

    python3 tools/synthctl.py --help
    python3 tools/synthctl.py capture 40 -f PROFILE     # serial capture (use PlatformIO's python for pyserial)

Each subcommand forwards its remaining arguments unchanged to the matching script, which also still runs standalone.
Third-party packages are imported lazily per subcommand, so `capture` needs only pyserial.
Install everything with: pip install -r tools/requirements.txt

tools/patch_ble_midi.py is deliberately not a subcommand: it is a PlatformIO extra_script run on every firmware build.
"""
import importlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _deps import require  # noqa: E402

# name -> (module, one-line help, third-party imports required)
COMMANDS = {
    "capture": ("serial_capture", "capture the ESP32-S3 USB serial log for N seconds", ("serial",)),
    "midi": ("midi_cli", "send/monitor MIDI, SysEx params and CCs for ESP32-S3 testing", ("mido", "rtmidi")),
    "oled-capture": ("mpk_oled_capture", "save the MPK mini MK4 screen framebuffer as PNG/PPM", ("mido", "rtmidi")),
    "gen-font": ("gen_oled_font", "generate the MK4 OLED glyph table header (MpkOledFont.h)", ("PIL",)),
    "gen-manual": ("generate_manual", "generate or verify the MPK Mini MK4 HTML manual", ()),
}


def usage():
    width = max(map(len, COMMANDS))
    lines = ["usage: synthctl.py <command> [args...]", "", "Helper scripts for the synth tools. Commands:", ""]
    lines += [f"  {name:<{width}}  {desc}" for name, (_, desc, _) in COMMANDS.items()]
    lines += ["", "Run `synthctl.py <command> --help` for a command's options.",
              "Missing packages: pip install -r tools/requirements.txt"]
    return "\n".join(lines)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] in ("-h", "--help"):
        print(usage())
        return 0
    cmd = argv[0]
    if cmd not in COMMANDS:
        print(f"synthctl.py: unknown command '{cmd}'\n\n{usage()}", file=sys.stderr)
        return 2
    module, _, deps = COMMANDS[cmd]
    require(*deps)
    sys.argv[0] = f"synthctl.py {cmd}"  # argparse prog name in the sub-script's help/usage
    mod = importlib.import_module(module)
    return mod.main(argv[1:])


if __name__ == "__main__":
    sys.exit(main())
