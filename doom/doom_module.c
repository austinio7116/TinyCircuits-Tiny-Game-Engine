/*
 * doom_module.c — MicroPython C module for DOOM on Thumby Color
 *
 * Unix: doom.init("path/to/doom1.wad")
 * ARM:  doom.init(("path/to/doom1.wad", file_obj, size))
 */

#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "doom_core.h"

/* From i_system.c — error message buffer */
extern char doom_error_msg[512];
/* From z_zone.c — zone size for debug */
extern int doom_zone_size_kb;

/* Keep references to prevent GC */
static mp_obj_t wad_file_ref = MP_OBJ_NULL;

/* doom.init(wad_path_or_tuple) -> None */
static mp_obj_t doom_mp_init(mp_obj_t arg) {
    const char *wad_path;

    if (mp_obj_is_type(arg, &mp_type_tuple)) {
        /* ARM mode: (path, file_obj, size) */
        size_t len;
        mp_obj_t *items;
        mp_obj_tuple_get(arg, &len, &items);
        if (len != 3) {
            mp_raise_ValueError(MP_ERROR_TEXT("Expected (path, file, size)"));
        }
        wad_path = mp_obj_str_get_str(items[0]);
        wad_file_ref = items[1];
        size_t wad_size = (size_t)mp_obj_get_int(items[2]);
        doom_core_set_wad_file((void *)items[1], wad_size);
    } else {
        /* Unix mode: just a path string */
        wad_path = mp_obj_str_get_str(arg);
    }

    int ret = doom_core_init(wad_path);
    if (ret != 0) {
        if (doom_error_msg[0]) {
            mp_raise_msg_varg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("DOOM(z%dKB): %s"), doom_zone_size_kb, doom_error_msg);
        } else {
            mp_raise_msg_varg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("DOOM init failed: %d"), ret);
        }
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(doom_mp_init_obj, doom_mp_init);

/* doom.load_cache((file_obj, size)) -> None */
static mp_obj_t doom_mp_load_cache(mp_obj_t arg) {
    if (mp_obj_is_type(arg, &mp_type_tuple)) {
        size_t len;
        mp_obj_t *items;
        mp_obj_tuple_get(arg, &len, &items);
        if (len == 2) {
            size_t cache_size = (size_t)mp_obj_get_int(items[1]);
#ifdef __arm__
            extern int doom_cache_load_mp(void *file_obj, size_t file_size);
            doom_cache_load_mp((void *)items[0], cache_size);
#else
            (void)cache_size;
#endif
        }
    } else {
        /* String path — use fopen */
        const char *path = mp_obj_str_get_str(arg);
        extern int doom_cache_load(const char *path);
        doom_cache_load(path);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(doom_mp_load_cache_obj, doom_mp_load_cache);

#ifdef __arm__
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "py/stream.h"

/* doom.flash_wad(file_obj, size) -> None
 * Write WAD file to raw flash at WAD_FLASH_OFFSET for XIP access. */
#define WAD_FLASH_OFFSET 0x200000  /* Must match w_file_xip.c */

static mp_obj_t doom_mp_flash_wad(mp_obj_t file_obj, mp_obj_t size_obj) {
    size_t wad_size = mp_obj_get_int(size_obj);
    const mp_stream_p_t *stream_p = mp_get_stream(file_obj);
    int errcode = 0;
    uint8_t buf[4096];

    /* Seek to start */
    struct mp_stream_seek_t seek_s = { .offset = 0, .whence = 0 };
    stream_p->ioctl(file_obj, MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, &errcode);

    mp_printf(&mp_plat_print, "Erasing flash at 0x%x (%dKB)...\n",
              WAD_FLASH_OFFSET, wad_size/1024);

    /* Erase in 4KB sectors */
    size_t erase_size = (wad_size + 4095) & ~4095;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WAD_FLASH_OFFSET, erase_size);
    restore_interrupts(ints);

    mp_printf(&mp_plat_print, "Programming %dKB...\n", wad_size/1024);

    /* Program in 4KB chunks */
    for (size_t off = 0; off < wad_size; off += sizeof(buf)) {
        size_t chunk = wad_size - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        /* Read from file */
        size_t total_read = 0;
        while (total_read < chunk) {
            size_t got = stream_p->read(file_obj, buf + total_read,
                                        chunk - total_read, &errcode);
            if (got == 0) break;
            total_read += got;
        }
        /* Pad remainder with 0xFF */
        if (total_read < sizeof(buf))
            memset(buf + total_read, 0xFF, sizeof(buf) - total_read);

        ints = save_and_disable_interrupts();
        flash_range_program(WAD_FLASH_OFFSET + off, buf, sizeof(buf));
        restore_interrupts(ints);

        if ((off & 0x1FFFF) == 0)
            mp_printf(&mp_plat_print, "  %d%%\n", (int)(off * 100 / wad_size));
    }

    mp_printf(&mp_plat_print, "WAD flashed to 0x%x (%dKB)\n",
              WAD_FLASH_OFFSET, wad_size/1024);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(doom_mp_flash_wad_obj, doom_mp_flash_wad);
#endif

/* doom.run_loop() -> int (frame count) */
static mp_obj_t doom_mp_run_loop(void) {
    int frames = doom_core_run_loop();
    if (doom_error_msg[0]) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("DOOM run: %s"), doom_error_msg);
    }
    return mp_obj_new_int(frames);
}
MP_DEFINE_CONST_FUN_OBJ_0(doom_mp_run_loop_obj, doom_mp_run_loop);

/* doom.deinit() -> None */
static mp_obj_t doom_mp_deinit(void) {
    doom_core_deinit();
    wad_file_ref = MP_OBJ_NULL;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(doom_mp_deinit_obj, doom_mp_deinit);

/* Module globals table */
static const mp_rom_map_elem_t doom_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_doom) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_init),     MP_ROM_PTR(&doom_mp_init_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_load_cache), MP_ROM_PTR(&doom_mp_load_cache_obj) },
#ifdef __arm__
    { MP_OBJ_NEW_QSTR(MP_QSTR_flash_wad), MP_ROM_PTR(&doom_mp_flash_wad_obj) },
#endif
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit),   MP_ROM_PTR(&doom_mp_deinit_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_run_loop), MP_ROM_PTR(&doom_mp_run_loop_obj) },
};

static MP_DEFINE_CONST_DICT(mp_module_doom_globals, doom_globals_table);

const mp_obj_module_t doom_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_doom_globals,
};

MP_REGISTER_MODULE(MP_QSTR_doom, doom_user_cmodule);
