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
PALETTE_NAMES = ["Green", "Gray", "Pocket"]

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
    path = ROM_DIR + "/" + filename
    with open(path, "rb") as f:
        return bytearray(f.read())

def save_path_for(rom_filename):
    base = rom_filename.rsplit('.', 1)[0]
    return SAVE_DIR + "/" + base + ".sav"

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
        with open(save_path_for(rom_filename), "wb") as f:
            f.write(data)
        print("GBEmu: Saved cart RAM", size, "bytes")

def load_cart_ram(rom_filename):
    path = save_path_for(rom_filename)
    try:
        with open(path, "rb") as f:
            data = f.read()
        gb_emu.set_cart_ram(data)
        print("GBEmu: Loaded cart RAM", len(data), "bytes")
    except OSError:
        pass

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

    # Truncate display names
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
def pause_menu(rom_filename, current_palette, audio_on):
    options = ["Resume", "Palette: " + PALETTE_NAMES[current_palette],
               "Audio: " + ("On" if audio_on else "Off"), "Reset", "Quit"]
    cursor = 0

    cam = CameraNode()

    bg = Rectangle2DNode(width=105, height=90, color=Color(0, 0, 0), position=Vector2(0, 2))
    bg.opacity = 0.85
    cam.add_child(bg)

    border = Rectangle2DNode(width=105, height=90, color=Color(1, 1, 1), outline=True, position=Vector2(0, 2))
    cam.add_child(border)

    title = Text2DNode(text="PAUSED", position=Vector2(0, -32))
    cam.add_child(title)

    items = []
    for i in range(len(options)):
        t = Text2DNode(text="", position=Vector2(0, -16 + i * 12))
        cam.add_child(t)
        items.append(t)

    def refresh():
        options[1] = "Palette: " + PALETTE_NAMES[current_palette]
        options[2] = "Audio: " + ("On" if audio_on else "Off")
        for i, opt in enumerate(options):
            prefix = "> " if i == cursor else "  "
            items[i].text = prefix + opt

    refresh()
    result = "resume"

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
                elif cursor == 0:
                    result = "resume"
                elif cursor == 1:
                    current_palette = (current_palette + 1) % len(PALETTE_NAMES)
                    gb_emu.set_palette(current_palette)
                    refresh()
                    continue
                elif cursor == 2:
                    audio_on = not audio_on
                    gb_emu.set_audio_enabled(audio_on)
                    refresh()
                    continue
                elif cursor == 3:
                    result = "reset"
                elif cursor == 4:
                    result = "quit"

                cam.mark_destroy_children()
                cam.mark_destroy()
                return result, current_palette, audio_on

# --- Main ---
roms = find_roms()
selected = rom_browser(roms)
if selected is None:
    engine.end()
else:
    rom_data = load_rom(selected)
    gb_emu.init(rom_data)
    current_palette = gb_emu.PALETTE_GREEN
    gb_emu.set_palette(current_palette)
    load_cart_ram(selected)

    rom_name = gb_emu.get_rom_name()
    print("GBEmu: Loaded", rom_name)
    gc.collect()

    engine.fps_limit(60)
    cam = CameraNode()

    audio_on = True
    running = True
    while running:
        if engine.tick():
            # Build joypad bitmask
            buttons = 0
            if engine_io.A.is_pressed:
                buttons |= gb_emu.BTN_A
            if engine_io.B.is_pressed:
                buttons |= gb_emu.BTN_B
            if engine_io.LB.is_pressed:
                buttons |= gb_emu.BTN_SELECT
            if engine_io.RB.is_pressed:
                buttons |= gb_emu.BTN_START
            if engine_io.UP.is_pressed:
                buttons |= gb_emu.BTN_UP
            if engine_io.DOWN.is_pressed:
                buttons |= gb_emu.BTN_DOWN
            if engine_io.LEFT.is_pressed:
                buttons |= gb_emu.BTN_LEFT
            if engine_io.RIGHT.is_pressed:
                buttons |= gb_emu.BTN_RIGHT

            gb_emu.set_buttons(buttons)
            gb_emu.run_frame()

            if engine_io.MENU.is_just_pressed:
                # Pause
                cam.mark_destroy()
                result, current_palette, audio_on = pause_menu(selected, current_palette, audio_on)

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
