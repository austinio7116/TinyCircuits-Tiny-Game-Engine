#!/usr/bin/env python3
"""
backup_device.py — Pull Games/, Saves/, system/ and main.py off a Thumby Color.

Usage:
    python3 backup_device.py <dest_dir> [--device /dev/ttyACM0]

Run BEFORE flashing a different firmware (e.g. the DOOM build) so you can
restore your games and saves afterwards with restore_device.py.
"""
import argparse
import os
import subprocess
import sys

DEFAULT_DEVICE = "/dev/ttyACM0"
TOP_LEVEL = ["Games", "Saves", "system"]
TOP_FILES = ["main.py"]


def mpremote(device, *args, capture=True):
    cmd = ["mpremote", "connect", device, *args]
    return subprocess.run(cmd, capture_output=capture, text=True)


def listdir(device, path):
    """Return (dirs, files) under :path on device."""
    r = mpremote(device, "ls", f":{path}")
    if r.returncode != 0:
        return [], []
    dirs, files = [], []
    for line in r.stdout.splitlines()[1:]:  # skip "ls :path"
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        size, name = parts
        if name.endswith("/"):
            dirs.append(name.rstrip("/"))
        else:
            files.append(name)
    return dirs, files


def pull_tree(device, remote_path, local_root):
    dirs, files = listdir(device, remote_path)
    local_dir = os.path.join(local_root, remote_path)
    os.makedirs(local_dir, exist_ok=True)
    for f in files:
        remote = f"{remote_path}/{f}"
        local = os.path.join(local_dir, f)
        print(f"  pull {remote}")
        r = mpremote(device, "cp", f":{remote}", local)
        if r.returncode != 0:
            print(f"    !! failed: {r.stderr.strip()}", file=sys.stderr)
    for d in dirs:
        pull_tree(device, f"{remote_path}/{d}", local_root)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dest")
    ap.add_argument("--device", default=DEFAULT_DEVICE)
    args = ap.parse_args()

    os.makedirs(args.dest, exist_ok=True)
    print(f"Backing up {args.device} -> {args.dest}")

    for top in TOP_LEVEL:
        print(f"[{top}/]")
        pull_tree(args.device, top, args.dest)

    for f in TOP_FILES:
        print(f"[{f}]")
        local = os.path.join(args.dest, f)
        r = mpremote(args.device, "cp", f":{f}", local)
        if r.returncode != 0:
            print(f"  skipped ({r.stderr.strip()})")

    print("Done.")


if __name__ == "__main__":
    main()
