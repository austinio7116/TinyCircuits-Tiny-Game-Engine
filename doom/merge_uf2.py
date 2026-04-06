#!/usr/bin/env python3
"""
merge_uf2.py — Merge two UF2 files into one, renumbering block counts.

Usage:
    python3 merge_uf2.py firmware.uf2 wad.uf2 combined.uf2
"""

import struct
import sys

def read_uf2_blocks(path):
    blocks = []
    with open(path, 'rb') as f:
        while True:
            block = f.read(512)
            if len(block) < 512:
                break
            blocks.append(bytearray(block))
    return blocks

def write_uf2(blocks, path):
    total = len(blocks)
    with open(path, 'wb') as f:
        for i, block in enumerate(blocks):
            # Update block number and total count
            struct.pack_into('<I', block, 20, i)      # blockNo
            struct.pack_into('<I', block, 24, total)   # numBlocks
            f.write(block)

if len(sys.argv) != 4:
    print(f"Usage: {sys.argv[0]} <firmware.uf2> <wad.uf2> <combined.uf2>")
    sys.exit(1)

fw_blocks = read_uf2_blocks(sys.argv[1])
wad_blocks = read_uf2_blocks(sys.argv[2])

print(f"Firmware: {len(fw_blocks)} blocks")
print(f"WAD: {len(wad_blocks)} blocks")

combined = fw_blocks + wad_blocks
write_uf2(combined, sys.argv[3])

print(f"Combined: {len(combined)} blocks ({len(combined)*512//1024}KB)")
print(f"Output: {sys.argv[3]}")
