#!/usr/bin/env python3
"""
flash_wad.py — Program a DOOM WAD into raw flash on a Thumby Color via mpremote.

Requires the DOOM firmware to be running on the device (it exposes the
`doom.flash_wad(file, size)` function which writes to flash offset 0x200000).

Usage:
    python3 flash_wad.py <wad_path> [--device /dev/ttyACM0]

The script:
  1. Copies <wad_path> onto the device as :doom1.wad
  2. Runs a tiny snippet that opens it and calls doom.flash_wad()
  3. Leaves the .wad file on the device filesystem (delete it manually if you
     want the space back — XIP doesn't need the filesystem copy).

The device will be unresponsive for ~10s while ~4 MB of flash is erased and
programmed. DO NOT UNPLUG.
"""
import argparse
import os
import subprocess
import sys

DEFAULT_DEVICE = "/dev/ttyACM0"


def mpremote(device, *args, **kw):
    return subprocess.run(["mpremote", "connect", device, *args],
                          capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wad")
    ap.add_argument("--device", default=DEFAULT_DEVICE)
    ap.add_argument("--remote-name", default="doom1.wad")
    args = ap.parse_args()

    if not os.path.isfile(args.wad):
        print(f"WAD not found: {args.wad}", file=sys.stderr)
        sys.exit(1)

    size = os.path.getsize(args.wad)
    print(f"WAD: {args.wad} ({size/1024:.1f} KB)")

    print(f"[1/2] Copying to :{args.remote_name} ...")
    r = mpremote(args.device, "cp", args.wad, f":{args.remote_name}")
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        sys.exit(1)

    snippet = (
        "import doom, os\n"
        f"f = open('{args.remote_name}', 'rb')\n"
        f"doom.flash_wad(f, os.stat('{args.remote_name}')[6])\n"
        "f.close()\n"
        "print('flash_wad ok')\n"
    )

    print("[2/2] Programming flash (do not unplug, ~10s) ...")
    r = mpremote(args.device, "exec", snippet)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0 or "flash_wad ok" not in r.stdout:
        sys.exit(1)

    print("Done. WAD is now mapped at 0x10200000 via XIP.")


if __name__ == "__main__":
    main()
