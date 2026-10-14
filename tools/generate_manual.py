#!/usr/bin/env python3
"""
generate_manual.py

Convenience runner to generate and verify the Akai MPK Mini MK4 HTML manual
against lib/SynthEngine/include/Presets.h, and to print it to a
US Letter PDF for offline reference.

Usage:
    python3 tools/generate_manual.py          # Generate / update HTML manual
    python3 tools/generate_manual.py --verify # Verify manual is in sync with Presets.h
    python3 tools/generate_manual.py --pdf    # Generate the HTML, then print build/mpk_mini_mk4_manual.pdf
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

CHROME_CANDIDATES = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "google-chrome",
    "google-chrome-stable",
    "chromium",
    "chromium-browser",
]


def find_chrome():
    override = os.environ.get("CHROME")
    for candidate in ([override] if override else []) + CHROME_CANDIDATES:
        path = candidate if os.path.isabs(candidate) else shutil.which(candidate)
        if path and os.path.isfile(path):
            return path
    return None


def print_pdf(html_path, pdf_path):
    """Print the manual with headless Chrome. Page size (US Letter) and margins come from the @page CSS rules."""
    chrome = find_chrome()
    if chrome is None:
        print("[generate_manual] Chrome/Chromium not found. Install Google Chrome or set CHROME=/path/to/chrome.",
              file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(pdf_path), exist_ok=True)
    if os.path.exists(pdf_path):
        os.remove(pdf_path)
    # A fresh --user-data-dir makes headless Chrome hang after printing, so use its default headless profile
    cmd = [
        chrome,
        "--headless",
        "--disable-gpu",
        "--no-pdf-header-footer",
        f"--print-to-pdf={pdf_path}",
        Path(html_path).resolve().as_uri(),
    ]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        print("[generate_manual] Chrome did not finish within 120 s.", file=sys.stderr)
        return 1
    if res.returncode != 0 or not os.path.isfile(pdf_path):
        print(res.stderr, file=sys.stderr)
        print("[generate_manual] PDF printing failed!", file=sys.stderr)
        return res.returncode or 1
    print(f"[generate_manual] Wrote {pdf_path}")
    return 0


def main(argv=None):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, ".."))
    build_dir = os.path.join(repo_root, "build")
    generator_bin = os.path.join(build_dir, "manual_generator")
    manual_path = os.path.join(repo_root, "docs", "mpk_mini_mk4_manual.html")
    default_pdf = os.path.join(build_dir, "mpk_mini_mk4_manual.pdf")

    ap = argparse.ArgumentParser(description="Generate or verify the Akai MPK Mini MK4 HTML manual.")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("-v", "--verify", action="store_true", help="verify the manual is in sync with Presets.h")
    mode.add_argument("--pdf", nargs="?", const=default_pdf, metavar="PATH",
                      help=f"also print a US Letter PDF (default: {os.path.relpath(default_pdf, repo_root)})")
    args = ap.parse_args(argv)

    # Build manual_generator via CMake (a no-op when it is up to date, so Presets.h edits are picked up)
    if not os.path.isfile(os.path.join(build_dir, "CMakeCache.txt")):
        print("[generate_manual] Configuring CMake...")
        res = subprocess.run(["cmake", "-B", build_dir, "-S", repo_root], cwd=repo_root)
        if res.returncode != 0:
            print("[generate_manual] CMake configuration failed!", file=sys.stderr)
            sys.exit(res.returncode)
    res = subprocess.run(["cmake", "--build", build_dir, "--target", "manual_generator"], cwd=repo_root)
    if res.returncode != 0 or not os.path.isfile(generator_bin):
        print("[generate_manual] Build failed!", file=sys.stderr)
        sys.exit(res.returncode or 1)

    cmd = [generator_bin, "--verify" if args.verify else "--write", manual_path]
    res = subprocess.run(cmd, cwd=repo_root)
    if res.returncode != 0 or not args.pdf:
        sys.exit(res.returncode)

    sys.exit(print_pdf(manual_path, os.path.abspath(args.pdf)))


if __name__ == "__main__":
    main()
