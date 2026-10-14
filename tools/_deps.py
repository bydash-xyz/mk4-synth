"""Shared dependency check for the tools/ scripts: one consistent message for a missing package."""
import importlib
import sys

# import name -> pip package name
PIP_NAMES = {"serial": "pyserial", "mido": "mido", "rtmidi": "python-rtmidi", "PIL": "pillow"}


def require(*modules):
    """Exit with a clean message if any of the given modules cannot be imported."""
    missing = []
    for m in modules:
        try:
            importlib.import_module(m)
        except ImportError:
            missing.append(PIP_NAMES.get(m, m))
    if missing:
        sys.exit(f"error: missing Python package(s): {', '.join(missing)}\n"
                 f"install with: pip install -r tools/requirements.txt")
