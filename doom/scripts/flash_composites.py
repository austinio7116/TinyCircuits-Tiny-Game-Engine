#!/usr/bin/env python3
"""
flash_composites.py — Program a DOOM texture composite blob into raw flash on
a Thumby Color via mpremote.

The blob is built by ../preprocess_composites.py from the same WAD that's
already flashed on the device. The runtime (r_data.c) maps it XIP at
0x10700000 and uses it to skip R_GenerateLookup / R_GenerateComposite — which
removes the texture-fragmentation OOM that crashes door-opening on E1M1.

The DOOM firmware must already be running on the device.

Usage:
    python3 flash_composites.py <blob.cmp> [--device /dev/ttyACM0]

The script:
  1. Copies <blob.cmp> onto the device as :doom1.cmp
  2. Runs `doom.flash_composites(open('doom1.cmp','rb'), os.stat(...)[6])`
  3. Leaves the .cmp file on the device filesystem (delete it manually if you
     want the space back — XIP doesn't need the filesystem copy).

The device will be unresponsive for several seconds while flash is erased and
programmed. DO NOT UNPLUG.
"""
import argparse
import os
import subprocess
import sys

DEFAULT_DEVICE = "/dev/ttyACM0"


def mpremote(device, *args):
    return subprocess.run(["mpremote", "connect", device, *args],
                          capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blob")
    ap.add_argument("--device", default=DEFAULT_DEVICE)
    ap.add_argument("--remote-name", default="doom1.cmp")
    args = ap.parse_args()

    if not os.path.isfile(args.blob):
        print(f"Blob not found: {args.blob}", file=sys.stderr)
        sys.exit(1)

    size = os.path.getsize(args.blob)
    print(f"Blob: {args.blob} ({size/1024:.1f} KB)")

    print(f"[1/2] Copying to :{args.remote_name} ...")
    r = mpremote(args.device, "cp", args.blob, f":{args.remote_name}")
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        sys.exit(1)

    snippet = (
        "import doom, os\n"
        f"f = open('{args.remote_name}', 'rb')\n"
        f"doom.flash_composites(f, os.stat('{args.remote_name}')[6])\n"
        "f.close()\n"
        "print('flash_composites ok')\n"
    )

    print("[2/2] Programming flash (do not unplug) ...")
    r = mpremote(args.device, "exec", snippet)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0 or "flash_composites ok" not in r.stdout:
        sys.exit(1)

    print("Done. Composites are now mapped at 0x10700000 via XIP.")


if __name__ == "__main__":
    main()
