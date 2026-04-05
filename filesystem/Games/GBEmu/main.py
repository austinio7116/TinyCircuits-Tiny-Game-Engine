import engine_main
import engine
import engine_io
import gb_emu
import os

# --- Configuration ---
ROM_DIR = "roms"
DEFAULT_PALETTE = gb_emu.PALETTE_GREEN

# --- ROM loading ---
def find_roms():
    """List .gb ROM files in the roms directory."""
    try:
        files = os.listdir(ROM_DIR)
    except OSError:
        return []
    return [f for f in files if f.lower().endswith('.gb')]

def load_rom(filename):
    """Load a ROM file into a bytearray."""
    path = ROM_DIR + "/" + filename
    with open(path, "rb") as f:
        return bytearray(f.read())

# --- Find and load first available ROM ---
roms = find_roms()
if not roms:
    # No ROMs found - show message and exit
    from engine_nodes import CameraNode, Text2DNode
    from engine_math import Vector2
    cam = CameraNode()
    t = Text2DNode(text="No .gb ROMs found", position=Vector2(0, -8))
    t2 = Text2DNode(text="Put ROMs in:", position=Vector2(0, 4))
    t3 = Text2DNode(text=ROM_DIR, position=Vector2(0, 16))
    cam.add_child(t)
    cam.add_child(t2)
    cam.add_child(t3)
    while True:
        if engine.tick():
            if engine_io.MENU.is_just_pressed:
                break
    engine.end()
else:
    # Load the first ROM (TODO: ROM browser in Phase 2)
    rom_data = load_rom(roms[0])
    gb_emu.init(rom_data)
    gb_emu.set_palette(DEFAULT_PALETTE)

    rom_name = gb_emu.get_rom_name()
    print("GBEmu: Loaded", rom_name)

    engine.fps_limit(60)

    # Minimal camera node required by engine
    from engine_nodes import CameraNode
    cam = CameraNode()

    # --- Main emulation loop ---
    while True:
        if engine.tick():
            # Build joypad bitmask from Thumby buttons
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

            # Run one GB frame - renders directly to framebuffer
            gb_emu.run_frame()

            # MENU exits back to launcher
            if engine_io.MENU.is_just_pressed:
                break

    engine.end()
