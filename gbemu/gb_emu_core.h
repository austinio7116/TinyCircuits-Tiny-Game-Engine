#ifndef GB_EMU_CORE_H
#define GB_EMU_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Peanut-GB configuration — must be defined before including peanut_gb.h */
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0

/* minigb_apu configuration */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 22050
#endif
#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif

/* Forward declarations for Peanut-GB audio callbacks (called implicitly by peanut_gb.h) */
#if ENABLE_SOUND
void audio_write(uint16_t addr, uint8_t val);
uint8_t audio_read(uint16_t addr);
#endif

/* Only gb_emu_core.c gets the Peanut-GB implementation.
 * All other files see only the declarations. */
#ifndef GB_EMU_CORE_IMPL
#define PEANUT_GB_HEADER_ONLY
#endif

#include "peanut_gb.h"

/* Palette presets */
#define GB_PALETTE_GREEN  0
#define GB_PALETTE_GRAY   1
#define GB_PALETTE_POCKET 2
#define GB_PALETTE_CREAM  3
#define GB_PALETTE_BLUE   4
#define GB_PALETTE_RED    5
#define GB_PALETTE_COUNT  6

/* Initialize the emulator with ROM data in memory. Returns 0 on success, negative on error. */
int gb_emu_init(uint8_t *rom_data, size_t rom_size);

/* Initialize the emulator reading ROM from file on demand (for large ROMs).
 * file_obj is a MicroPython file object (must stay open). */
int gb_emu_init_file(void *file_obj, size_t rom_size);

/* Run one full frame (~70224 CPU cycles). Renders to active_screen_buffer. */
void gb_emu_run_frame(void);

/* Set joypad state (use JOYPAD_* bitmask from peanut_gb.h). */
void gb_emu_set_buttons(uint8_t buttons);

/* Reset the GB CPU. */
void gb_emu_reset(void);

/* Get ROM title string. Writes into buf (must be >= 17 bytes). */
const char *gb_emu_get_rom_name(char *buf);

/* Set display palette (GB_PALETTE_GREEN, GB_PALETTE_GRAY, GB_PALETTE_POCKET). */
void gb_emu_set_palette(int palette_index);

/* Get pointer to cart RAM and its size (for save/load). */
uint8_t *gb_emu_get_cart_ram(size_t *size_out);

/* Set cart RAM data (for loading saves). */
void gb_emu_set_cart_ram(const uint8_t *data, size_t size);

/* Get the save size (0 if no cart RAM). */
size_t gb_emu_get_save_size(void);

/* Audio: get next sample for engine mixing. Returns float -1.0 to 1.0, or 0 if no data. */
float gb_emu_get_audio_sample(void);

/* Audio: enable/disable audio output */
void gb_emu_set_audio_enabled(int enabled);

/* Crop offset: pan the 128x128 viewport within 160x144 GB screen */
void gb_emu_set_crop(int x, int y);
int gb_emu_get_crop_x(void);
int gb_emu_get_crop_y(void);

/* Run emulation loop entirely in C until MENU pressed. Returns frame count. */
int gb_emu_run_loop(void);

/* Show/hide FPS counter overlay */
void gb_emu_set_show_fps(int show);

/* Frame skip: render every other frame (CPU/audio still run each frame) */
void gb_emu_set_frame_skip(int enabled);

/* Save states: snapshot/restore full emulator state */
size_t gb_emu_get_state_size(void);
int gb_emu_save_state(uint8_t *buf, size_t buf_size);
int gb_emu_load_state(const uint8_t *buf, size_t buf_size);

/* Free dynamically allocated buffers (cart_ram, bank cache, audio ring).
 * Call when exiting the emulator to release memory for other games. */
void gb_emu_deinit(void);

#endif /* GB_EMU_CORE_H */
