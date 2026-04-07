# DOOM on Thumby Color

A native C-module port of **doomgeneric** running on the TinyCircuits Thumby Color
(RP2350, 128×128 RGB565, 520 KB SRAM, 16 MB flash). The shareware WAD lives in
raw flash and is read directly via XIP — zero-copy lump access, no heap WAD load.

This README is the build/deploy guide. For the architecture / memory-budget
plan see `../../DOOM_PLAN.md` history; this document supersedes it.

---

## What works

- Title screen, menus, fully playable E1M1 (movement, shooting, enemies,
  item pickups, automap).
- WAD streamed from raw flash via XIP at `0x10200000` — no heap copy.
- Native 128×128 rendering (no 320×200 → downscale buffers).
- Long-press MENU exits cleanly back to the launcher; short-press opens the
  in-game DOOM menu.
- Errors inside DOOM (`I_Error`) are caught with `setjmp/longjmp` and surfaced
  to MicroPython instead of killing the interpreter.

## Known limitations

- **No audio.** All sound functions are stubbed.
- **No screen wipes** (saved ~33 KB BSS).
- **No savegames.**
- **Opening the first big door on E1M1 OOMs the zone heap.** The door itself
  renders fine; the crash happens once it opens and the room beyond is
  revealed — most likely a texture composite (or several) for the new
  geometry / enemies needs a contiguous block the fragmented zone can't
  provide. Reported cleanly via `I_Error`. This is the next thing to fix.
- **Intro demos don't run.** The title screen comes up but the attract-mode
  demo playback is broken; it falls through to the menu instead.
- **Shareware `doom1.wad` only** has been tested. Bigger IWADs need a larger
  flash partition and almost certainly more zone heap.

## Controls

| Button | Action |
|---|---|
| D-pad UP / DOWN | Forward / back |
| D-pad LEFT / RIGHT | Turn left / right |
| A | Fire (also Enter in menus) |
| B | Use / open doors |
| LB | Strafe left |
| RB | Strafe right |
| MENU (short press) | Open DOOM menu (Escape) |
| MENU (long press) | Exit DOOM, return to launcher |

---

## Repository layout

Everything DOOM-specific lives under
`TinyCircuits-Tiny-Game-Engine/doom/`:

```
doom/
  micropython.cmake     # Registers DOOM as a usermod and pulls in doomgeneric/*.c
  doom_module.c         # MicroPython module: doom.init/run_loop/deinit/flash_wad
  doom_core.c / .h      # Platform glue: input, framebuffer, run_loop
  doom_cache.c / .h     # XIP lump pointer cache
  doom_config.h         # Per-platform tunables
  w_file_xip.c          # WAD file backend reading directly from XIP flash
  doomgeneric/          # Vendored, heavily patched doomgeneric source tree
  make_wad_uf2.py       # Build a UF2 that flashes a WAD to 0x10200000
  preprocess_wad.py     # (optional) extract init lumps for faster load
  merge_uf2.py          # Merge firmware.uf2 + wad.uf2 into one drop-in UF2
  scripts/
    backup_device.py    # Pull Games/, Saves/, system/, main.py off the device
    restore_device.py   # Push backup back, optionally excluding Doom
    flash_wad.py        # Stream a .wad file to flash via mpremote (no BOOTSEL)
```

The Python launcher game ships in the engine repo at
`TinyCircuits-Tiny-Game-Engine/filesystem/Games/Doom/` (`main.py` + `icon.bmp`)
so it shows up in the launcher and gets baked into the firmware build.

---

## How the port works (short version)

1. **128×128 native rendering.** `SCREENWIDTH/HEIGHT` are set to 128 and every
   array sized off them is shrunk accordingly. This is the single biggest win:
   `MAXWIDTH`/`MAXHEIGHT` go from 1120/832 to 128/128, killing tens of KB of BSS.
2. **Aggressive BSS reduction.** `MAXVISPLANES`, `MAXLIGHTSCALE`, `MAXLIGHTZ`,
   `MAXEVENTS`, `viewangletox` (now `int16_t`) all trimmed. Total BSS is now
   ~72 KB (was ~192 KB).
3. **XIP WAD.** `w_file_xip.c` ignores `fopen` and just hands DOOM a pointer to
   `0x10200000`. `wad_file_t.mapped` is set so every `W_CacheLumpNum` returns
   a flash pointer with no copy. The WAD is written to that flash region either
   by the `doom.flash_wad()` Python helper, or by flashing a UF2 generated from
   `make_wad_uf2.py`.
4. **Zone heap from MicroPython GC.** `i_system.c` allocates the DOOM zone via
   `m_malloc_maybe` (the MicroPython GC heap), starting at 256 KB and stepping
   down 4 KB at a time until it fits. Freed completely on `doom.deinit()`.
5. **Error recovery.** All `I_Error` paths longjmp back to `doom_core.c`, which
   propagates the message to a MicroPython exception. No more silent dies.
6. **Direct framebuffer.** On device, `I_FinishUpdate` writes RGB565 directly
   into the engine's `active_screen_buffer`. On the Unix emulator there's a
   conversion path from doomgeneric's RGBA8888 buffer.
7. **No audio, no wipes, no savegame** — all stubbed.

The port uses `DOOM_THUMBY` as the build define for the device-specific
codepaths. The same source builds on Linux for development.

---

## Memory map (RP2350, 16 MB flash)

| Region | Offset | Size | Purpose |
|---|---|---|---|
| Firmware | `0x000000` | 1 MB | MicroPython + engine + DOOM module |
| **WAD** | **`0x200000`** | **~4.2 MB** | **Shareware `doom1.wad`** |
| LittleFS | `0x800000` | 8 MB | Filesystem (`/Games`, `/Saves`, etc.) |

The standard (non-DOOM) firmware ships with a 13 MB filesystem. The DOOM
firmware **must** be built with an 8 MB filesystem so the WAD region isn't
clobbered. This is a one-line change in
`mp-thumby/ports/rp2/boards/THUMBY_COLOR/mpconfigboard.h`:

```c
#define MICROPY_HW_FLASH_STORAGE_BYTES (8 * 1024 * 1024)
```

Restore it to `(13 * 1024 * 1024)` before building any non-DOOM firmware.

---

## Prerequisites

You need the same toolchain as a normal Thumby Color firmware build, plus
`mpremote` for deployment.

```bash
sudo apt install git python3 cmake gcc-arm-none-eabi libnewlib-arm-none-eabi \
                 build-essential g++ libstdc++-arm-none-eabi-newlib
pip3 install mpremote pyserial
```

You also need the TinyCircuits MicroPython fork as a sibling checkout at
`../mp-thumby` (branch `engine`, submodules initialized, mpy-cross built).
This is the same setup as any other engine build — see the top-level
`CLAUDE.md`.

You need a copy of **`doom1.wad`** (the shareware IWAD). It ships with every
DOOM release going back to 1993; place it somewhere accessible like
`~/wads/doom1.wad`.

---

## Step-by-step: build and deploy DOOM to a Thumby Color

> ⚠️ **Back up first.** Flashing a different firmware leaves your filesystem
> mostly intact, but writing a WAD to raw flash overwrites part of it, and
> you'll be replacing system files too. Always pull a backup first.

### 1. Connect the device (WSL2)

```powershell
# Windows PowerShell (Admin), once per session:
usbipd list
usbipd bind   --busid <BUSID>   # find the 2E8A:0005 device
usbipd attach --wsl --busid <BUSID>
```

Close Thonny / any Windows app holding the COM port before running `attach`.
The device should now appear as `/dev/ttyACM0` in WSL.

### 2. Back up the device

```bash
cd TinyCircuits-Tiny-Game-Engine/doom/scripts
python3 backup_device.py /home/you/thumby_backup
```

This pulls `Games/`, `Saves/`, `system/` and `main.py` off the device. Keep
this directory safe — it is how you'll restore everything afterwards.

### 3. Build the DOOM firmware (or use the prebuilt one)

A prebuilt UF2 is checked into this branch at
`doom/dist/thumby-color-doom-firmware.uf2`. If you use that, skip straight to
step 4 — but note you must still push the matching `system/` and `main.py`
from this same branch's `filesystem/` tree in step 5, or the launcher will
hit a digest mismatch.

To build it yourself:

```bash
cd TinyCircuits-Tiny-Game-Engine

# Set the filesystem partition to 8 MB so the WAD region is free:
sed -i 's/(13 \* 1024 \* 1024)/(8 * 1024 * 1024)/' \
    ../mp-thumby/ports/rp2/boards/THUMBY_COLOR/mpconfigboard.h

python3 build_and_upload.py no_upload

# Restore the partition size for future non-DOOM builds:
sed -i 's/(8 \* 1024 \* 1024)/(13 * 1024 * 1024)/' \
    ../mp-thumby/ports/rp2/boards/THUMBY_COLOR/mpconfigboard.h
```

The firmware lands at `mp-thumby/ports/rp2/build-THUMBY_COLOR/firmware.uf2`.

### 4. Flash the firmware

1. Power the Thumby Color OFF.
2. Hold **D-pad DOWN** and turn it ON — it mounts as an `RP2350` USB drive
   in Windows.
3. Drag `firmware.uf2` onto that drive. It reboots automatically.

### 5. Push the matching system files

The launcher's `system_files_digest` is regenerated on every firmware build
and baked into the firmware. The actual `system/` files and `main.py` are
**not** auto-deployed — you must copy them onto the filesystem yourself after
every firmware flash, or the launcher will refuse to start with a digest
mismatch. Push them from the build tree you just compiled:

```bash
cd TinyCircuits-Tiny-Game-Engine
mpremote connect /dev/ttyACM0 cp filesystem/main.py :main.py
mpremote connect /dev/ttyACM0 cp -r filesystem/system :
```

### 6. Restore games and saves

```bash
cd doom/scripts
python3 restore_device.py /home/you/thumby_backup
```

`restore_device.py` skips `system/` and `main.py` by default (you just pushed
those from the firmware tree in step 5), and skips `Games/Doom` because the
firmware build already laid that down from
`filesystem/Games/Doom/`. Use `--include-system` if you want it to push the
backed-up versions instead.

### 7. Flash the WAD

Use `flash_wad.py`. It calls `doom.flash_wad()` on the running firmware,
which programs the WAD to raw flash at `0x200000` via `flash_range_program`
and prints a percentage as it goes:

```bash
cd TinyCircuits-Tiny-Game-Engine/doom/scripts
python3 flash_wad.py /path/to/doom1.wad
```

The script copies the WAD to the device filesystem, then runs a snippet that
opens it and calls `doom.flash_wad()`. Erasing and programming ~4.2 MB takes
a little while; **do not unplug** until it finishes.

> Note: there is also a `make_wad_uf2.py` that produces a BOOTSEL-droppable
> UF2 targeting `0x10200000`. In practice that route did **not** work
> reliably on the Thumby Color — use `flash_wad.py` instead. The UF2 helper
> is left in the tree for reference / future debugging.

### 8. Play

Launch **Doom** from the games menu. It shows a black screen with a red
loading bar for ~5–10 s while DOOM does its zone init and texture lookup,
then drops into the title screen.

To exit cleanly: long-press **MENU**.

---

## Going back to the standard firmware

1. Make sure your backup from step 2 is still around.
2. Build the normal firmware. The only DOOM-specific change to revert is the
   filesystem size in `mpconfigboard.h` (which step 3 already restored). The
   normal firmware does **not** include the `doom/` user module unless you
   explicitly include it from the top-level `micropython.cmake`.
3. Flash it via BOOTSEL the same way as step 4.
4. Push the matching system files and your backup with steps 5 (system files)
   and `restore_device.py --include-doom` if you also want the in-firmware
   Doom launcher entry restored. Note: the Doom entry only does anything if
   the firmware has the doom module compiled in.

The WAD region in flash is harmless to leave behind; nothing reads it unless
the DOOM firmware is loaded.

---

## Helper scripts (in `doom/scripts/`)

- **`backup_device.py <dest>`** — recursively `mpremote cp` everything under
  `/Games`, `/Saves`, `/system`, plus `/main.py`, into `<dest>`.
- **`restore_device.py <src> [--include-doom] [--include-system]`** — push a
  backup back to the device. Excludes `system/`, `main.py` and `Games/Doom`
  by default (those should be pushed from the matching firmware build tree
  instead, since the firmware bakes in a digest of the system files).
- **`flash_wad.py <wad>`** — copy a WAD onto the device tmp area and call
  `doom.flash_wad()` to program it into raw flash at `0x200000`. Requires the
  DOOM firmware to be running.
- **`make_wad_uf2.py <wad> <out.uf2>`** (in `doom/`, not `scripts/`) — produce
  a BOOTSEL-droppable UF2 that flashes the WAD without needing the DOOM
  firmware to be running.

---

## Troubleshooting

- **`system_files_digest mismatch`** at boot — you flashed a new firmware but
  forgot to push the matching `system/` and `main.py` (step 5). Re-run that.
- **`Z_Malloc: failed`** — zone fragmentation, see "Known limitations".
  Most reliably triggered by opening the first big door on E1M1. Restarting
  the game gives you a fresh zone.
- **`doom.flash_wad` looks hung** — it isn't. Erasing and programming 4 MB
  of flash on the RP2350 takes several seconds. Don't unplug.
- **Black screen forever** — the loading bar is drawn directly to the panel.
  If you get past the bar but never see the title screen, capture
  `/last_crash.txt` from the device and look for `I_Error:` lines.
- **Device won't appear in `usbipd list`** — try a different USB-C cable.
  Charge-only cables are extremely common and will not work.
