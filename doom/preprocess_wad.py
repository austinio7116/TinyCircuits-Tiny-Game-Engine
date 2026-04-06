#!/usr/bin/env python3
"""
preprocess_wad.py — Pre-extract DOOM WAD init data for fast loading on Thumby Color.

Reads DOOM1.WAD and creates doom_cache.bin containing all lumps needed during
engine init (textures, sprites, palette, colormap, etc). This avoids hundreds
of slow individual file reads on the device.

Usage:
    python3 preprocess_wad.py doom1.wad doom_cache.bin

Cache format:
    Header:  "DCACHE01"  (8 bytes magic)
             uint32 num_lumps
             uint32 data_offset (start of lump data blob)
    Index:   For each lump:
             char[8] name
             uint32 offset_in_data
             uint32 size
    Data:    Concatenated lump data
"""

import struct
import sys
import os

def read_wad(wad_path):
    """Read WAD directory and return (lumps, file_handle)."""
    f = open(wad_path, 'rb')

    # WAD header
    magic = f.read(4)
    if magic not in (b'IWAD', b'PWAD'):
        raise ValueError(f"Not a WAD file: {magic}")

    num_lumps = struct.unpack('<I', f.read(4))[0]
    dir_offset = struct.unpack('<I', f.read(4))[0]

    # Read directory
    f.seek(dir_offset)
    lumps = []
    for i in range(num_lumps):
        pos = struct.unpack('<I', f.read(4))[0]
        size = struct.unpack('<I', f.read(4))[0]
        name = f.read(8).rstrip(b'\x00').decode('ascii', errors='replace')
        lumps.append({'name': name, 'pos': pos, 'size': size, 'index': i})

    return lumps, f

def find_lumps_between(lumps, start_name, end_name):
    """Find all lumps between start_name and end_name markers."""
    result = []
    in_range = False
    for l in lumps:
        if l['name'] == start_name:
            in_range = True
            continue
        if l['name'] == end_name:
            break
        if in_range:
            result.append(l)
    return result

def get_init_lumps(lumps):
    """Determine which lumps DOOM reads during init."""
    needed = set()

    # Always needed for init
    for name in ['PLAYPAL', 'COLORMAP', 'TEXTURE1', 'TEXTURE2', 'PNAMES',
                 'GENMIDI', 'DMXGUS', 'ENDOOM',
                 # Status bar
                 'STBAR', 'STARMS', 'STTMINUS',
                 # Menu/title patches
                 'M_DOOM', 'M_NEWG', 'M_SKILL', 'M_EPISOD', 'M_OPTTTL',
                 'M_SAVEG', 'M_LOADG', 'M_RDTHIS', 'M_QUITG',
                 'M_NGAME', 'M_OPTION', 'M_SGTTL', 'M_LGTTL',
                 'M_PAUSE', 'M_SKULL1', 'M_SKULL2',
                 'M_THERMM', 'M_THERML', 'M_THERMR', 'M_THERMO',
                 'M_EPI1', 'M_EPI2', 'M_EPI3',
                 'M_JKILL', 'M_ROUGH', 'M_HURT', 'M_ULTRA', 'M_NMARE',
                 # HUD font
                 'WIMAP0',
                 # Title screens
                 'TITLEPIC', 'CREDIT', 'HELP1', 'HELP2',
                 ]:
        needed.add(name)

    # Status bar number patches (STTNUM0-9)
    for i in range(10):
        needed.add(f'STTNUM{i}')

    # Status bar percent, face patches
    needed.add('STTPRCNT')
    for i in range(6):  # ST faces
        for j in range(3):
            needed.add(f'STFST{i}{j}')

    # Arms numbers
    for i in range(2, 8):
        needed.add(f'STGNUM{i}')
        needed.add(f'STYSNUM{i}')

    # HUD font (STCFN033-STCFN095)
    for i in range(33, 96):
        needed.add(f'STCFN{i:03d}')

    # Key patches
    for color in ['B', 'R', 'Y']:
        needed.add(f'STKEYS{color}')
        needed.add(f'STK{color}')
    needed.update([f'STKEYS{i}' for i in range(6)])

    # All patches referenced by PNAMES (needed for R_InitTextures)
    # We'll add ALL patches between P_START/P_END and S_START/S_END
    sprite_lumps = find_lumps_between(lumps, 'S_START', 'S_END')
    patch_lumps = find_lumps_between(lumps, 'P_START', 'P_END')

    for l in sprite_lumps:
        needed.add(l['name'])
    for l in patch_lumps:
        needed.add(l['name'])

    # Flat names (between F_START/F_END) — just names, not data
    # Actually R_InitFlats doesn't read flat data during init

    # Filter to lumps that exist
    result = []
    lump_by_name = {}
    for l in lumps:
        if l['name'] not in lump_by_name:
            lump_by_name[l['name']] = l

    # Add all needed lumps that exist
    for name in needed:
        if name in lump_by_name:
            result.append(lump_by_name[name])

    # Also add ALL lumps — the device needs them all eventually,
    # and having them pre-cached avoids slow individual reads.
    # Actually, that would make the cache too big. Just do init lumps.

    return result

def build_cache(wad_path, cache_path):
    lumps, f = read_wad(wad_path)
    init_lumps = get_init_lumps(lumps)

    print(f"WAD: {len(lumps)} total lumps")
    print(f"Cache: {len(init_lumps)} init lumps")

    # Read lump data
    cache_entries = []
    data_blob = bytearray()

    for l in init_lumps:
        if l['size'] == 0:
            cache_entries.append((l['name'], len(data_blob), 0))
            continue
        f.seek(l['pos'])
        data = f.read(l['size'])
        cache_entries.append((l['name'], len(data_blob), len(data)))
        data_blob.extend(data)

    f.close()

    print(f"Cache data: {len(data_blob)} bytes ({len(data_blob)//1024}KB)")

    # Build cache file
    # Header: magic(8) + num_entries(4) + data_offset(4) = 16 bytes
    # Index: num_entries * (name(8) + offset(4) + size(4)) = 16 bytes each
    header_size = 16
    index_size = len(cache_entries) * 16
    data_offset = header_size + index_size

    with open(cache_path, 'wb') as out:
        # Header
        out.write(b'DCACHE01')
        out.write(struct.pack('<I', len(cache_entries)))
        out.write(struct.pack('<I', data_offset))

        # Index
        for name, offset, size in cache_entries:
            name_bytes = name.encode('ascii')[:8].ljust(8, b'\x00')
            out.write(name_bytes)
            out.write(struct.pack('<I', offset))
            out.write(struct.pack('<I', size))

        # Data
        out.write(data_blob)

    total = data_offset + len(data_blob)
    print(f"Cache file: {total} bytes ({total//1024}KB)")
    print(f"Saved to: {cache_path}")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <doom1.wad> <doom_cache.bin>")
        sys.exit(1)
    build_cache(sys.argv[1], sys.argv[2])
