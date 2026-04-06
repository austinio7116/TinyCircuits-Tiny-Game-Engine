#include "py/obj.h"
#include "py/runtime.h"
#include "py/objarray.h"
#include "py/objstr.h"
#include "gb_emu_core.h"

/* Keep a reference to the ROM file object to prevent GC */
static mp_obj_t rom_file_ref = MP_OBJ_NULL;

/* gb_emu.init(rom_bytearray_or_file_tuple) -> None
 * Initialize with ROM data (bytearray) or (file_obj, size) tuple for on-demand reading. */
static mp_obj_t gb_emu_mp_init(mp_obj_t rom_obj) {
    int ret;

    if (mp_obj_is_type(rom_obj, &mp_type_tuple)) {
        /* Tuple mode: (file_obj, size) for file-based reading */
        size_t len;
        mp_obj_t *items;
        mp_obj_tuple_get(rom_obj, &len, &items);
        if (len != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("Expected (file, size) tuple"));
        }
        rom_file_ref = items[0];  /* prevent GC of file object */
        size_t rom_size = (size_t)mp_obj_get_int(items[1]);
        ret = gb_emu_init_file((void *)items[0], rom_size);
    } else {
        /* Bytearray mode: ROM in memory */
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(rom_obj, &bufinfo, MP_BUFFER_READ);

        if (bufinfo.len < 0x150) {
            mp_raise_ValueError(MP_ERROR_TEXT("ROM too small"));
        }

        ret = gb_emu_init((uint8_t *)bufinfo.buf, bufinfo.len);
    }

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

/* gb_emu.set_show_fps(bool) -> None */
static mp_obj_t gb_emu_mp_set_show_fps(mp_obj_t show_obj) {
    gb_emu_set_show_fps(mp_obj_is_true(show_obj) ? 1 : 0);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_set_show_fps_obj, gb_emu_mp_set_show_fps);

/* gb_emu.run_loop() -> int (frame count)
 * High-performance all-C frame loop. Runs until MENU pressed. */
static mp_obj_t gb_emu_mp_run_loop(void) {
    int frames = gb_emu_run_loop();
    return mp_obj_new_int(frames);
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_run_loop_obj, gb_emu_mp_run_loop);

/* gb_emu.deinit() -> None
 * Free dynamic buffers (cart RAM, bank cache, audio ring). */
static mp_obj_t gb_emu_mp_deinit(void) {
    gb_emu_deinit();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_deinit_obj, gb_emu_mp_deinit);

/* gb_emu.set_frame_skip(bool) -> None */
static mp_obj_t gb_emu_mp_set_frame_skip(mp_obj_t enabled_obj) {
    gb_emu_set_frame_skip(mp_obj_is_true(enabled_obj) ? 1 : 0);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_set_frame_skip_obj, gb_emu_mp_set_frame_skip);

/* gb_emu.set_audio_enabled(bool) -> None */
static mp_obj_t gb_emu_mp_set_audio_enabled(mp_obj_t enabled_obj) {
    gb_emu_set_audio_enabled(mp_obj_is_true(enabled_obj) ? 1 : 0);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_set_audio_enabled_obj, gb_emu_mp_set_audio_enabled);

/* gb_emu.set_crop(x, y) -> None */
static mp_obj_t gb_emu_mp_set_crop(mp_obj_t x_obj, mp_obj_t y_obj) {
    gb_emu_set_crop(mp_obj_get_int(x_obj), mp_obj_get_int(y_obj));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(gb_emu_mp_set_crop_obj, gb_emu_mp_set_crop);

/* gb_emu.get_crop() -> (x, y) */
static mp_obj_t gb_emu_mp_get_crop(void) {
    mp_obj_t items[2] = {
        mp_obj_new_int(gb_emu_get_crop_x()),
        mp_obj_new_int(gb_emu_get_crop_y()),
    };
    return mp_obj_new_tuple(2, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_get_crop_obj, gb_emu_mp_get_crop);

/* gb_emu.save_state() -> bytearray or None on error */
static mp_obj_t gb_emu_mp_save_state(void) {
    size_t size = gb_emu_get_state_size();
    /* Allocate zeroed buffer first, then fill */
    uint8_t *buf = m_new(uint8_t, size);
    if (!buf) return mp_const_none;
    int ret = gb_emu_save_state(buf, size);
    if (ret != 0) {
        m_del(uint8_t, buf, size);
        return mp_const_none;
    }
    mp_obj_t result = mp_obj_new_bytearray(size, buf);
    m_del(uint8_t, buf, size);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(gb_emu_mp_save_state_obj, gb_emu_mp_save_state);

/* gb_emu.load_state(bytearray) -> bool */
static mp_obj_t gb_emu_mp_load_state(mp_obj_t data_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    int ret = gb_emu_load_state(bufinfo.buf, bufinfo.len);
    return mp_obj_new_bool(ret == 0);
}
MP_DEFINE_CONST_FUN_OBJ_1(gb_emu_mp_load_state_obj, gb_emu_mp_load_state);

/* Module globals table */
static const mp_rom_map_elem_t gb_emu_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__),       MP_OBJ_NEW_QSTR(MP_QSTR_gb_emu) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_init),            MP_ROM_PTR(&gb_emu_mp_init_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit),          MP_ROM_PTR(&gb_emu_mp_deinit_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_run_frame),       MP_ROM_PTR(&gb_emu_mp_run_frame_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_buttons),     MP_ROM_PTR(&gb_emu_mp_set_buttons_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_reset),           MP_ROM_PTR(&gb_emu_mp_reset_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_rom_name),    MP_ROM_PTR(&gb_emu_mp_get_rom_name_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_palette),     MP_ROM_PTR(&gb_emu_mp_set_palette_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_cart_ram),    MP_ROM_PTR(&gb_emu_mp_get_cart_ram_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_cart_ram),    MP_ROM_PTR(&gb_emu_mp_set_cart_ram_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_save_size),   MP_ROM_PTR(&gb_emu_mp_get_save_size_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_run_loop),          MP_ROM_PTR(&gb_emu_mp_run_loop_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_show_fps),    MP_ROM_PTR(&gb_emu_mp_set_show_fps_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_audio_enabled), MP_ROM_PTR(&gb_emu_mp_set_audio_enabled_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_frame_skip),  MP_ROM_PTR(&gb_emu_mp_set_frame_skip_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_crop),        MP_ROM_PTR(&gb_emu_mp_set_crop_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_crop),        MP_ROM_PTR(&gb_emu_mp_get_crop_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_save_state),      MP_ROM_PTR(&gb_emu_mp_save_state_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_load_state),      MP_ROM_PTR(&gb_emu_mp_load_state_obj) },
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
    { MP_OBJ_NEW_QSTR(MP_QSTR_PALETTE_CREAM),   MP_OBJ_NEW_SMALL_INT(GB_PALETTE_CREAM) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PALETTE_BLUE),    MP_OBJ_NEW_SMALL_INT(GB_PALETTE_BLUE) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PALETTE_RED),     MP_OBJ_NEW_SMALL_INT(GB_PALETTE_RED) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PALETTE_COUNT),   MP_OBJ_NEW_SMALL_INT(GB_PALETTE_COUNT) },
};

static MP_DEFINE_CONST_DICT(mp_module_gb_emu_globals, gb_emu_globals_table);

const mp_obj_module_t gb_emu_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_gb_emu_globals,
};

MP_REGISTER_MODULE(MP_QSTR_gb_emu, gb_emu_user_cmodule);
