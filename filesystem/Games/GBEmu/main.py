import engine_main
import engine
import engine_io
import engine_draw
import gb_emu
import os
import gc

from engine_nodes import CameraNode, Text2DNode, Rectangle2DNode
from engine_math import Vector2
from engine_draw import Color

# --- Configuration ---
ROM_DIR = "roms"
SAVE_DIR = "saves"
STATE_DIR = "saves"
PALETTE_NAMES = ["Green", "Gray", "Pocket", "Cream", "Blue", "Red"]
PAN_SPEED = 2  # pixels per frame when panning

# --- Helpers ---
def find_roms():
    try:
        files = os.listdir(ROM_DIR)
    except OSError:
        return []
    roms = [f for f in files if f.lower().endswith('.gb') or f.lower().endswith('.gbc')]
    roms.sort()
    return roms

def load_rom(filename):
    gc.collect()
    path = ROM_DIR + "/" + filename
    size = os.stat(path)[6]
    try:
        buf = bytearray(size)
        with open(path, "rb") as f:
            f.readinto(buf)
        return buf
    except MemoryError:
        # ROM too large for heap — use file-based on-demand reading
        print("GBEmu: ROM too large for RAM, using file streaming")
        f = open(path, "rb")
        return (f, size)  # return (file_obj, size) tuple for C init

def base_name(rom_filename):
    return rom_filename.rsplit('.', 1)[0]

def ensure_dir(path):
    try:
        os.mkdir(path)
    except OSError:
        pass

def save_cart_ram(rom_filename):
    size = gb_emu.get_save_size()
    if size == 0:
        return
    ensure_dir(SAVE_DIR)
    data = gb_emu.get_cart_ram()
    if data:
        with open(SAVE_DIR + "/" + base_name(rom_filename) + ".sav", "wb") as f:
            f.write(data)

def load_cart_ram(rom_filename):
    try:
        with open(SAVE_DIR + "/" + base_name(rom_filename) + ".sav", "rb") as f:
            gb_emu.set_cart_ram(f.read())
    except OSError:
        pass

def save_state(rom_filename):
    ensure_dir(STATE_DIR)
    data = gb_emu.save_state()
    if data:
        with open(STATE_DIR + "/" + base_name(rom_filename) + ".state", "wb") as f:
            f.write(data)
        return True
    return False

def load_state(rom_filename):
    try:
        with open(STATE_DIR + "/" + base_name(rom_filename) + ".state", "rb") as f:
            data = f.read()
        return gb_emu.load_state(data)
    except OSError:
        return False

def has_save_state(rom_filename):
    try:
        os.stat(STATE_DIR + "/" + base_name(rom_filename) + ".state")
        return True
    except OSError:
        return False

# --- ROM Browser ---
def rom_browser(roms):
    engine.fps_limit(30)
    cam = CameraNode()

    title = Text2DNode(text="GB Emulator", position=Vector2(0, -52))
    cam.add_child(title)

    if not roms:
        msg = Text2DNode(text="No ROMs found", position=Vector2(0, -8))
        msg2 = Text2DNode(text="Put .gb files in:", position=Vector2(0, 4))
        msg3 = Text2DNode(text=ROM_DIR + "/", position=Vector2(0, 16))
        cam.add_child(msg)
        cam.add_child(msg2)
        cam.add_child(msg3)
        while True:
            if engine.tick():
                if engine_io.MENU.is_just_pressed:
                    cam.mark_destroy_children()
                    cam.mark_destroy()
                    return None
        return None

    cursor = 0
    scroll = 0
    max_visible = 7
    item_h = 12

    def display_name(f):
        name = f.rsplit('.', 1)[0]
        if len(name) > 18:
            name = name[:16] + ".."
        return name

    hint = Text2DNode(text="A:Select  MENU:Back", position=Vector2(0, 52))
    cam.add_child(hint)

    items = []
    for i in range(max_visible):
        t = Text2DNode(text="", position=Vector2(0, -30 + i * item_h))
        cam.add_child(t)
        items.append(t)

    def refresh():
        for i in range(max_visible):
            idx = scroll + i
            if idx < len(roms):
                prefix = "> " if idx == cursor else "  "
                items[i].text = prefix + display_name(roms[idx])
                items[i].opacity = 1.0
            else:
                items[i].text = ""
                items[i].opacity = 0.0

    refresh()

    while True:
        if engine.tick():
            moved = False
            if engine_io.DOWN.is_just_pressed:
                if cursor < len(roms) - 1:
                    cursor += 1
                    if cursor >= scroll + max_visible:
                        scroll += 1
                    moved = True
            elif engine_io.UP.is_just_pressed:
                if cursor > 0:
                    cursor -= 1
                    if cursor < scroll:
                        scroll -= 1
                    moved = True

            if moved:
                refresh()

            if engine_io.A.is_just_pressed:
                selected = roms[cursor]
                cam.mark_destroy_children()
                cam.mark_destroy()
                return selected

            if engine_io.MENU.is_just_pressed:
                cam.mark_destroy_children()
                cam.mark_destroy()
                return None

# --- Pause Menu ---
def pause_menu(rom_filename, current_palette, audio_on, fps_on, fskip_on):
    has_state = has_save_state(rom_filename)
    options = [
        "Resume",
        "Palette: " + PALETTE_NAMES[current_palette],
        "Audio: " + ("On" if audio_on else "Off"),
        "FPS: " + ("On" if fps_on else "Off"),
        "FrameSkip: " + ("On" if fskip_on else "Off"),
        "Save State",
        "Load State" + ("" if has_state else " (none)"),
        "Reset",
        "Quit",
    ]
    cursor = 0

    cam = CameraNode()

    menu_h = 120
    bg = Rectangle2DNode(width=110, height=menu_h, color=Color(0, 0, 0), position=Vector2(0, 0))
    bg.opacity = 0.85
    cam.add_child(bg)

    border = Rectangle2DNode(width=110, height=120, color=Color(1, 1, 1), outline=True, position=Vector2(0, 0))
    cam.add_child(border)

    title = Text2DNode(text="PAUSED", position=Vector2(0, -42))
    cam.add_child(title)

    items = []
    for i in range(len(options)):
        t = Text2DNode(text="", position=Vector2(0, -26 + i * 10))
        cam.add_child(t)
        items.append(t)

    def refresh():
        options[1] = "Palette: " + PALETTE_NAMES[current_palette]
        options[2] = "Audio: " + ("On" if audio_on else "Off")
        options[3] = "FPS: " + ("On" if fps_on else "Off")
        options[4] = "FrameSkip: " + ("On" if fskip_on else "Off")
        for i, opt in enumerate(options):
            prefix = "> " if i == cursor else "  "
            items[i].text = prefix + opt

    refresh()
    result = "resume"

    # Burn ticks until MENU is released (avoid instant re-trigger)
    while True:
        if engine.tick():
            if not engine_io.MENU.is_pressed:
                break

    while True:
        if engine.tick():
            moved = False
            if engine_io.DOWN.is_just_pressed:
                cursor = (cursor + 1) % len(options)
                moved = True
            elif engine_io.UP.is_just_pressed:
                cursor = (cursor - 1) % len(options)
                moved = True

            if moved:
                refresh()

            if engine_io.A.is_just_pressed or engine_io.MENU.is_just_pressed:
                if engine_io.MENU.is_just_pressed:
                    result = "resume"
                elif cursor == 0:  # Resume
                    result = "resume"
                elif cursor == 1:  # Palette
                    current_palette = (current_palette + 1) % gb_emu.PALETTE_COUNT
                    gb_emu.set_palette(current_palette)
                    refresh()
                    continue
                elif cursor == 2:  # Audio
                    audio_on = not audio_on
                    gb_emu.set_audio_enabled(audio_on)
                    refresh()
                    continue
                elif cursor == 3:  # FPS
                    fps_on = not fps_on
                    gb_emu.set_show_fps(fps_on)
                    refresh()
                    continue
                elif cursor == 4:  # Frame Skip
                    fskip_on = not fskip_on
                    gb_emu.set_frame_skip(fskip_on)
                    refresh()
                    continue
                elif cursor == 5:  # Save State
                    if save_state(rom_filename):
                        items[5].text = "> State Saved!"
                    else:
                        items[5].text = "> Save Failed!"
                    continue
                elif cursor == 6:  # Load State
                    if load_state(rom_filename):
                        result = "resume"
                    else:
                        items[6].text = "> No State Found"
                        continue
                elif cursor == 7:  # Reset
                    result = "reset"
                elif cursor == 8:  # Quit
                    result = "quit"

                cam.mark_destroy_children()
                cam.mark_destroy()
                return result, current_palette, audio_on, fps_on, fskip_on

# --- Main ---
roms = find_roms()
selected = rom_browser(roms)
if selected is None:
    engine.end()
else:
    gc.collect()
    rom_data = load_rom(selected)
    gb_emu.init(rom_data)
    current_palette = gb_emu.PALETTE_GREEN
    gb_emu.set_palette(current_palette)
    load_cart_ram(selected)

    rom_name = gb_emu.get_rom_name()
    print("GBEmu: Loaded", rom_name)
    gc.collect()

    engine.freq(250 * 1000 * 1000)  # Overclock to 250MHz for file-streaming performance
    engine.fps_limit(60)
    cam = CameraNode()

    audio_on = True
    fps_on = False
    fskip_on = False
    running = True
    while running:
        # Run emulation entirely in C until MENU is pressed
        gb_emu.run_loop()

        # MENU was pressed — show pause menu
        cam.mark_destroy()
        result, current_palette, audio_on, fps_on, fskip_on = pause_menu(selected, current_palette, audio_on, fps_on, fskip_on)

        if result == "resume":
            cam = CameraNode()
        elif result == "reset":
            gb_emu.reset()
            cam = CameraNode()
        elif result == "quit":
            save_cart_ram(selected)
            running = False

    save_cart_ram(selected)
    engine.end()
