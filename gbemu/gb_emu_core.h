#ifndef GB_EMU_CORE_H
#define GB_EMU_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Peanut-GB configuration — must be defined before including peanut_gb.h */
#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0

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
#define GB_PALETTE_COUNT  3

/* Initialize the emulator with ROM data. Returns 0 on success, negative on error. */
int gb_emu_init(uint8_t *rom_data, size_t rom_size);

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

#endif /* GB_EMU_CORE_H */
