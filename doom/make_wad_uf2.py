#!/usr/bin/env python3
"""
make_wad_uf2.py — Convert DOOM WAD to UF2 file for direct flash programming.

The UF2 file targets flash offset 0x100000 (1MB) on RP2350.
Flash the WAD by entering BOOTSEL mode and copying the .uf2 file.

Usage:
    python3 make_wad_uf2.py doom1.wad doom1_wad.uf2
"""

import struct
import sys

# UF2 constants
UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30
UF2_FLAG_FAMILY  = 0x00002000

# RP2350 family ID (0xe48bff57, NOT 0xe48bff56 which is RP2040)
RP2350_FAMILY_ID = 0xe48bff57

# Target flash address (XIP base + offset)
WAD_FLASH_ADDR = 0x10200000  # XIP base 0x10000000 + 2MB offset (must match w_file_xip.c)

# UF2 payload size per block
PAYLOAD_SIZE = 256

def make_uf2(input_path, output_path):
    with open(input_path, 'rb') as f:
        data = f.read()

    print(f"Input: {input_path} ({len(data)} bytes, {len(data)//1024}KB)")

    # Pad to 256-byte alignment
    if len(data) % PAYLOAD_SIZE:
        data += b'\xff' * (PAYLOAD_SIZE - len(data) % PAYLOAD_SIZE)

    num_blocks = len(data) // PAYLOAD_SIZE
    print(f"UF2 blocks: {num_blocks}")
    print(f"Target flash addr: 0x{WAD_FLASH_ADDR:08X}")

    with open(output_path, 'wb') as out:
        for i in range(num_blocks):
            offset = i * PAYLOAD_SIZE
            addr = WAD_FLASH_ADDR + offset
            payload = data[offset:offset + PAYLOAD_SIZE]

            # UF2 block: 512 bytes total
            # Header: 32 bytes
            header = struct.pack('<IIIIIIII',
                UF2_MAGIC_START0,
                UF2_MAGIC_START1,
                UF2_FLAG_FAMILY,    # flags
                addr,               # target address
                PAYLOAD_SIZE,       # payload size
                i,                  # block number
                num_blocks,         # total blocks
                RP2350_FAMILY_ID,   # family ID
            )
            # Payload: 476 bytes (256 data + 220 padding)
            block = header + payload + b'\x00' * (476 - PAYLOAD_SIZE)
            # Footer: 4 bytes
            block += struct.pack('<I', UF2_MAGIC_END)

            assert len(block) == 512
            out.write(block)

    total_size = num_blocks * 512
    print(f"Output: {output_path} ({total_size} bytes, {total_size//1024}KB)")
    print(f"\nTo flash: enter BOOTSEL mode and copy {output_path} to the RP2350 drive")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <doom1.wad> <output.uf2>")
        sys.exit(1)
    make_uf2(sys.argv[1], sys.argv[2])
