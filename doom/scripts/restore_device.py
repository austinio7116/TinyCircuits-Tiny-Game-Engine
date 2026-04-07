#!/usr/bin/env python3
"""
restore_device.py — Push a backup made by backup_device.py back to a Thumby Color.

Usage:
    python3 restore_device.py <backup_dir> [--device /dev/ttyACM0]
                              [--include-doom] [--include-system]

By default skips:
  - system/   (push these from the matching firmware build's filesystem/system)
  - main.py   (push from the matching firmware build's filesystem/main.py)
  - Games/Doom (the DOOM firmware lays this down from filesystem/Games/Doom)

The system files and main.py must match the digest baked into the firmware,
so always push them from the same firmware tree you just flashed — not from
an old backup.
"""
import argparse
import os
import subprocess
import sys

DEFAULT_DEVICE = "/dev/ttyACM0"


def mpremote(device, *args):
    return subprocess.run(["mpremote", "connect", device, *args],
                          capture_output=True, text=True)


def ensure_dir(device, remote_path):
    # mkdir is idempotent-ish: ignore "already exists"
    mpremote(device, "mkdir", f":{remote_path}")


def push_tree(device, local_root, rel, skip_dirs):
    local_dir = os.path.join(local_root, rel)
    if not os.path.isdir(local_dir):
        return
    if rel:
        ensure_dir(device, rel)
    for entry in sorted(os.listdir(local_dir)):
        local = os.path.join(local_dir, entry)
        rel_child = f"{rel}/{entry}" if rel else entry
        if os.path.isdir(local):
            if rel_child in skip_dirs:
                print(f"  skip {rel_child}/")
                continue
            push_tree(device, local_root, rel_child, skip_dirs)
        else:
            print(f"  push {rel_child}")
            r = mpremote(device, "cp", local, f":{rel_child}")
            if r.returncode != 0:
                print(f"    !! {r.stderr.strip()}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--device", default=DEFAULT_DEVICE)
    ap.add_argument("--include-doom", action="store_true",
                    help="Also restore Games/Doom (normally ships in firmware)")
    ap.add_argument("--include-system", action="store_true",
                    help="Also restore system/ and main.py (normally from firmware)")
    args = ap.parse_args()

    skip_dirs = set()
    if not args.include_doom:
        skip_dirs.add("Games/Doom")
    if not args.include_system:
        skip_dirs.add("system")

    print(f"Restoring {args.src} -> {args.device}")

    # Top-level directories
    for top in ("Games", "Saves", "system"):
        if top == "system" and not args.include_system:
            continue
        print(f"[{top}/]")
        push_tree(args.device, args.src, top, skip_dirs)

    # Top-level files
    if args.include_system:
        local_main = os.path.join(args.src, "main.py")
        if os.path.isfile(local_main):
            print("[main.py]")
            r = mpremote(args.device, "cp", local_main, ":main.py")
            if r.returncode != 0:
                print(f"  !! {r.stderr.strip()}", file=sys.stderr)

    print("Done.")


if __name__ == "__main__":
    main()
