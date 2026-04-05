#include "py/obj.h"
#include "py/runtime.h"
#include "py/objarray.h"
#include "py/objstr.h"
#include "gb_emu_core.h"

/* gb_emu.init(rom_bytearray) -> None
 * Initialize the emulator with ROM data.
 * The bytearray must remain alive for the lifetime of the emulator. */
static mp_obj_t gb_emu_mp_init(mp_obj_t rom_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(rom_obj, &bufinfo, MP_BUFFER_READ);

    if (bufinfo.len < 0x150) {
        mp_raise_ValueError(MP_ERROR_TEXT("ROM too small (need >= 336 bytes for header)"));
    }

    int ret = gb_emu_init((uint8_t *)bufinfo.buf, bufinfo.len);
    if (ret != 0) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("gb_init failed with error %d"), -ret);
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_init_obj, gb_emu_mp_init);

/* gb_emu.run_frame() -> None
 * Run one full Game Boy frame. Renders to the engine's active screen buffer. */
static mp_obj_t gb_emu_mp_run_frame(void) {
    gb_emu_run_frame();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_run_frame_obj, gb_emu_mp_run_frame);

/* gb_emu.set_buttons(mask) -> None
 * Set joypad button state. Use JOYPAD_* constants. */
static mp_obj_t gb_emu_mp_set_buttons(mp_obj_t buttons_obj) {
    uint8_t buttons = (uint8_t)mp_obj_get_int(buttons_obj);
    gb_emu_set_buttons(buttons);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_set_buttons_obj, gb_emu_mp_set_buttons);

/* gb_emu.reset() -> None */
static mp_obj_t gb_emu_mp_reset(void) {
    gb_emu_reset();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_reset_obj, gb_emu_mp_reset);

/* gb_emu.get_rom_name() -> str */
static mp_obj_t gb_emu_mp_get_rom_name(void) {
    char buf[17];
    gb_emu_get_rom_name(buf);
    return mp_obj_new_str(buf, strlen(buf));
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_get_rom_name_obj, gb_emu_mp_get_rom_name);

/* gb_emu.set_palette(index) -> None
 * 0=green, 1=grayscale, 2=pocket */
static mp_obj_t gb_emu_mp_set_palette(mp_obj_t idx_obj) {
    int idx = mp_obj_get_int(idx_obj);
    gb_emu_set_palette(idx);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_set_palette_obj, gb_emu_mp_set_palette);

/* gb_emu.get_cart_ram() -> bytearray (copy of cart RAM) */
static mp_obj_t gb_emu_mp_get_cart_ram(void) {
    size_t size = 0;
    uint8_t *ram = gb_emu_get_cart_ram(&size);
    if (size == 0)
        return mp_const_none;
    /* Return a copy so Python can save it to file */
    return mp_obj_new_bytearray(size, ram);
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_get_cart_ram_obj, gb_emu_mp_get_cart_ram);

/* gb_emu.set_cart_ram(bytearray) -> None */
static mp_obj_t gb_emu_mp_set_cart_ram(mp_obj_t data_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    gb_emu_set_cart_ram((const uint8_t *)bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_set_cart_ram_obj, gb_emu_mp_set_cart_ram);

/* gb_emu.get_save_size() -> int */
static mp_obj_t gb_emu_mp_get_save_size(void) {
    return mp_obj_new_int(gb_emu_get_save_size());
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_get_save_size_obj, gb_emu_mp_get_save_size);

/* Module globals table */
static const mp_rom_map_elem_t gb_emu_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__),       MP_OBJ_NEW_QSTR(MP_QSTR_gb_emu) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_init),            MP_ROM_PTR(&gb_emu_mp_init_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_run_frame),       MP_ROM_PTR(&gb_emu_mp_run_frame_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_buttons),     MP_ROM_PTR(&gb_emu_mp_set_buttons_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_reset),           MP_ROM_PTR(&gb_emu_mp_reset_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_rom_name),    MP_ROM_PTR(&gb_emu_mp_get_rom_name_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_palette),     MP_ROM_PTR(&gb_emu_mp_set_palette_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_cart_ram),    MP_ROM_PTR(&gb_emu_mp_get_cart_ram_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_cart_ram),    MP_ROM_PTR(&gb_emu_mp_set_cart_ram_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_save_size),   MP_ROM_PTR(&gb_emu_mp_get_save_size_obj) },
    /* Joypad constants for Python convenience */
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_A),           MP_OBJ_NEW_SMALL_INT(JOYPAD_A) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_B),           MP_OBJ_NEW_SMALL_INT(JOYPAD_B) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_SELECT),      MP_OBJ_NEW_SMALL_INT(JOYPAD_SELECT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_START),       MP_OBJ_NEW_SMALL_INT(JOYPAD_START) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_RIGHT),       MP_OBJ_NEW_SMALL_INT(JOYPAD_RIGHT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_LEFT),        MP_OBJ_NEW_SMALL_INT(JOYPAD_LEFT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_UP),          MP_OBJ_NEW_SMALL_INT(JOYPAD_UP) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BTN_DOWN),        MP_OBJ_NEW_SMALL_INT(JOYPAD_DOWN) },
    /* Palette constants */
    { MP_OBJ_NEW_QSTR(MP_QSTR_PALETTE_GREEN),   MP_OBJ_NEW_SMALL_INT(GB_PALETTE_GREEN) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PALETTE_GRAY),    MP_OBJ_NEW_SMALL_INT(GB_PALETTE_GRAY) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PALETTE_POCKET),  MP_OBJ_NEW_SMALL_INT(GB_PALETTE_POCKET) },
};

static MP_DEFINE_CONST_DICT(mp_module_gb_emu_globals, gb_emu_globals_table);

const mp_obj_module_t gb_emu_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_gb_emu_globals,
};

MP_REGISTER_MODULE(MP_QSTR_gb_emu, gb_emu_user_cmodule);
