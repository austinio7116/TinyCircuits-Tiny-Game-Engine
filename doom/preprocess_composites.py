#!/usr/bin/env python3
"""
preprocess_composites.py — Build a flash-resident composite blob for a DOOM WAD.

Input:  a DOOM WAD (e.g. doom1.wad)
Output: a .cmp blob containing, for every texture in the WAD:
          - column-lump table  (int16[width])
          - column-offset table (uint16[width])
          - composite pixel data (column-major, height bytes per multi-patch column)

The runtime (r_data.c on ARM) loads the blob at a fixed XIP flash address and
uses it to skip R_GenerateLookup and R_GenerateComposite entirely. The bytes
this script writes for composites are designed to be byte-identical to what
R_GenerateComposite would build at runtime, with the texture cache zero-
initialised. r_data.c has a matching DOOM_DUMP_COMPOSITES oracle that prints
CRC32s of every composite it generates so we can verify equality on the
emulator before trusting the blob on device.

Blob format (all little-endian, see r_data.c texture_flash_blob_init):

  HEADER (32 bytes):
    uint32 magic            'DCMP'  (0x504D4344)
    uint16 version          1
    uint16 numtextures
    uint32 wad_size         length of source WAD
    uint32 wad_first4       first 4 bytes of WAD ('IWAD')
    uint32 wad_last4        last 4 bytes of WAD
    uint32 table_offset     = 32
    uint32 total_size       total blob size including header

  PER-TEXTURE TABLE (numtextures * 16 bytes):
    uint16 width
    uint16 height
    uint32 columnlump_offset
    uint32 columnofs_offset
    uint32 composite_offset (0xFFFFFFFF if texture has no composite)

  DATA SECTION:
    For each texture: int16[width] columnlump
    For each texture: uint16[width] columnofs
    For each multi-patch texture: byte[csize] composite pixels

Usage:
    python3 preprocess_composites.py <wad> <output.cmp>
"""

import struct
import sys
import zlib


# ---------------- WAD parser ----------------

class Wad:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        magic, numlumps, infotableofs = struct.unpack_from("<4sII", self.data, 0)
        if magic not in (b"IWAD", b"PWAD"):
            raise SystemExit(f"{path}: not a WAD")
        self.numlumps = numlumps
        self.lumps = []  # list of (name, offset, size)
        for i in range(numlumps):
            off = infotableofs + i * 16
            l_offset, l_size = struct.unpack_from("<II", self.data, off)
            name = self.data[off + 8:off + 16].rstrip(b"\x00").decode("ascii", "replace")
            self.lumps.append((name, l_offset, l_size))

    def lump_data(self, idx):
        name, off, size = self.lumps[idx]
        return self.data[off:off + size]

    def find_lump(self, name):
        """Reverse-scan, mirroring W_CheckNumForName so the last occurrence wins."""
        target = name.upper()
        for i in range(len(self.lumps) - 1, -1, -1):
            if self.lumps[i][0].upper() == target:
                return i
        return -1

    def lump_data_by_name(self, name):
        idx = self.find_lump(name)
        if idx < 0:
            raise KeyError(name)
        return self.lump_data(idx)


# ---------------- Patch decoder ----------------

def parse_patch_header(buf):
    """Returns (width, height, leftoffset, topoffset, columnofs[width])."""
    width, height, lo, to = struct.unpack_from("<hhhh", buf, 0)
    columnofs = list(struct.unpack_from(f"<{width}I", buf, 8))
    return width, height, lo, to, columnofs


def draw_column_in_cache(patch_buf, col_ofs, cache, originy, cacheheight):
    """Mirror of R_DrawColumnInCache. Writes posts from a patch column into cache.

    `cache` is a bytearray of length cacheheight. The patch column starts at
    patch_buf[col_ofs] in post format: (topdelta, length, pad, length pixels, pad).
    Posts are concatenated, terminated by topdelta == 0xff.
    """
    p = col_ofs
    while True:
        topdelta = patch_buf[p]
        if topdelta == 0xff:
            return
        length = patch_buf[p + 1]
        # source = patch + 3   (skip topdelta, length, pad)
        source_off = p + 3
        position = originy + topdelta
        count = length

        if position < 0:
            count += position
            position = 0
        if position + count > cacheheight:
            count = cacheheight - position

        if count > 0:
            cache[position:position + count] = patch_buf[source_off:source_off + count]

        # Advance: patch += length + 4 (3 header bytes + length pixels + 1 pad)
        p += length + 4


# ---------------- Texture parser (mirrors R_InitTextures) ----------------

class Texture:
    __slots__ = ("name", "width", "height", "patches")

    def __init__(self, name, width, height, patches):
        self.name = name
        self.width = width
        self.height = height
        self.patches = patches  # list of (originx, originy, lumpnum)


def parse_textures(wad):
    """Mirror of R_InitTextures. Returns list[Texture]."""
    pnames = wad.lump_data_by_name("PNAMES")
    nummappatches = struct.unpack_from("<I", pnames, 0)[0]
    patchlookup = []
    for i in range(nummappatches):
        raw = pnames[4 + i * 8:4 + i * 8 + 8]
        name = raw.rstrip(b"\x00").decode("ascii", "replace").upper()
        patchlookup.append(wad.find_lump(name))

    def parse_one_texture_lump(lump_name):
        out = []
        if wad.find_lump(lump_name) < 0:
            return out
        buf = wad.lump_data_by_name(lump_name)
        numtex = struct.unpack_from("<i", buf, 0)[0]
        directory = struct.unpack_from(f"<{numtex}i", buf, 4)
        for ofs in directory:
            # maptexture_t: char name[8]; int masked; short width; short height;
            #               int obsolete; short patchcount; mappatch_t patches[];
            name = buf[ofs:ofs + 8].rstrip(b"\x00").decode("ascii", "replace")
            width, height = struct.unpack_from("<hh", buf, ofs + 12)
            patchcount = struct.unpack_from("<h", buf, ofs + 20)[0]
            patches = []
            for j in range(patchcount):
                # mappatch_t: short originx, originy, patch, stepdir, colormap;
                base = ofs + 22 + j * 10
                ox, oy, p_idx = struct.unpack_from("<hhh", buf, base)
                patches.append((ox, oy, patchlookup[p_idx]))
            out.append(Texture(name, width, height, patches))
        return out

    textures = parse_one_texture_lump("TEXTURE1")
    textures += parse_one_texture_lump("TEXTURE2")
    return textures


# ---------------- Lookup + composite generation ----------------
# Mirror of R_GenerateLookup + R_GenerateComposite.

def build_lookup_and_composite(wad, tex):
    """For one texture, returns (collump[width], colofs[width], composite_bytes_or_None).

    collump[c] = -1 if column c is composite, else patch lump number.
    colofs[c]  = offset within composite OR within patch lump.
    composite_bytes is None if no column needs compositing.
    """
    width = tex.width
    height = tex.height
    collump = [0] * width
    colofs = [0] * width
    patchcount = [0] * width

    # First pass: count patches per column, prefill single-patch entries.
    for (origx, origy, lumpnum) in tex.patches:
        p = wad.lump_data(lumpnum)
        pw, ph, plo, pto, pcols = parse_patch_header(p)
        x1 = origx
        x2 = origx + pw
        x = max(0, x1)
        end = min(x2, width)
        while x < end:
            patchcount[x] += 1
            collump[x] = lumpnum
            colofs[x] = pcols[x - x1] + 3  # skip post header to first pixel
            x += 1

    # Second pass: compute composite size, assign offsets for multi-patch columns.
    composite_size = 0
    for x in range(width):
        if patchcount[x] == 0:
            # "column without a patch" — DOOM prints a warning and continues;
            # collump stays 0, colofs stays 0, renderer reads garbage. We mirror.
            continue
        if patchcount[x] > 1:
            collump[x] = -1
            colofs[x] = composite_size
            if composite_size > 0x10000 - height:
                raise SystemExit(f"texture {tex.name}: composite >64KB")
            composite_size += height

    if composite_size == 0:
        return collump, colofs, None

    # Third pass: build composite by drawing every patch into the cache.
    # Cache is column-major: column c lives in cache[colofs[c] : colofs[c]+height].
    # draw_column_in_cache writes to a column-local buffer, so we work on a
    # per-column scratch slice and copy it back.
    cache = bytearray(composite_size)
    for (origx, origy, lumpnum) in tex.patches:
        p = wad.lump_data(lumpnum)
        pw, ph, plo, pto, pcols = parse_patch_header(p)
        x1 = origx
        x2 = origx + pw
        x = max(0, x1)
        end = min(x2, width)
        while x < end:
            if collump[x] >= 0:
                x += 1
                continue
            patch_col_off = pcols[x - x1]
            scratch = cache[colofs[x]:colofs[x] + height]  # bytearray slice = copy
            draw_column_in_cache(p, patch_col_off, scratch, origy, height)
            cache[colofs[x]:colofs[x] + height] = scratch
            x += 1

    return collump, colofs, bytes(cache)


# ---------------- Blob writer ----------------

MAGIC = 0x504D4344  # 'DCMP'
VERSION = 1
HEADER_FMT = "<IHHIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
ENTRY_FMT = "<HHIII"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
NO_COMPOSITE = 0xFFFFFFFF


def main():
    if len(sys.argv) != 3:
        print("usage: preprocess_composites.py <wad> <out.cmp>", file=sys.stderr)
        sys.exit(2)
    wad_path, out_path = sys.argv[1], sys.argv[2]

    wad = Wad(wad_path)
    textures = parse_textures(wad)
    print(f"{wad_path}: {len(textures)} textures")

    # Build per-texture data.
    per_tex = []  # list of (tex, collump, colofs, composite_bytes_or_none)
    total_lookup_bytes = 0
    total_composite_bytes = 0
    multi_patch_count = 0
    for tex in textures:
        cl, co, cmp_bytes = build_lookup_and_composite(wad, tex)
        per_tex.append((tex, cl, co, cmp_bytes))
        total_lookup_bytes += tex.width * 4  # int16 + uint16
        if cmp_bytes is not None:
            total_composite_bytes += len(cmp_bytes)
            multi_patch_count += 1

    print(f"  multi-patch textures: {multi_patch_count}")
    print(f"  lookup tables: {total_lookup_bytes/1024:.1f} KB")
    print(f"  composite bytes: {total_composite_bytes/1024:.1f} KB")

    # Lay out the blob.
    table_off = HEADER_SIZE
    data_off = table_off + ENTRY_SIZE * len(textures)

    # First pass: assign offsets.
    cur = data_off
    cl_offsets = []
    co_offsets = []
    cmp_offsets = []
    # All collump arrays first, then all colofs, then all composites.
    for (tex, cl, co, cmp_bytes) in per_tex:
        cl_offsets.append(cur)
        cur += tex.width * 2  # int16
    for (tex, cl, co, cmp_bytes) in per_tex:
        co_offsets.append(cur)
        cur += tex.width * 2  # uint16
    for (tex, cl, co, cmp_bytes) in per_tex:
        if cmp_bytes is None:
            cmp_offsets.append(NO_COMPOSITE)
        else:
            cmp_offsets.append(cur)
            cur += len(cmp_bytes)
    total_size = cur

    blob = bytearray(total_size)

    # Header.
    wad_first4 = struct.unpack("<I", wad.data[0:4])[0]
    wad_last4 = struct.unpack("<I", wad.data[-4:])[0]
    struct.pack_into(HEADER_FMT, blob, 0,
                     MAGIC, VERSION, len(textures),
                     len(wad.data), wad_first4, wad_last4,
                     table_off, total_size)

    # Per-texture table.
    for i, (tex, cl, co, cmp_bytes) in enumerate(per_tex):
        struct.pack_into(ENTRY_FMT, blob, table_off + i * ENTRY_SIZE,
                         tex.width, tex.height,
                         cl_offsets[i], co_offsets[i], cmp_offsets[i])

    # Data section.
    for i, (tex, cl, co, cmp_bytes) in enumerate(per_tex):
        struct.pack_into(f"<{tex.width}h", blob, cl_offsets[i], *cl)
    for i, (tex, cl, co, cmp_bytes) in enumerate(per_tex):
        struct.pack_into(f"<{tex.width}H", blob, co_offsets[i], *co)
    for i, (tex, cl, co, cmp_bytes) in enumerate(per_tex):
        if cmp_bytes is not None:
            blob[cmp_offsets[i]:cmp_offsets[i] + len(cmp_bytes)] = cmp_bytes

    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"Wrote {out_path}: {total_size/1024:.1f} KB")

    # Print CRC oracle: name + composite_size + crc32, for cross-checking.
    print()
    print("CRC oracle (texture, composite_size, crc32):")
    for (tex, cl, co, cmp_bytes) in per_tex:
        if cmp_bytes is not None:
            crc = zlib.crc32(cmp_bytes) & 0xffffffff
            print(f"  {tex.name:<8} {len(cmp_bytes):6d} {crc:08x}")


if __name__ == "__main__":
    main()
