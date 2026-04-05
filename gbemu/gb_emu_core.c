#define GB_EMU_CORE_IMPL
#include "gb_emu_core.h"
#include <string.h>
#include <stdio.h>

/* Engine's active framebuffer — defined in engine_display_common.c */
extern uint16_t *active_screen_buffer;

/* GB state */
static struct gb_s gb;
static uint8_t *rom_ptr = NULL;
static size_t rom_len = 0;
static uint8_t cart_ram[0x8000]; /* 32KB max cart RAM */
static int initialized = 0;

/* Display crop offsets: center 128x128 in 160x144 */
#define GB_SCREEN_W  160
#define GB_SCREEN_H  144
#define THUMBY_W     128
#define THUMBY_H     128
#define CROP_X       ((GB_SCREEN_W - THUMBY_W) / 2)  /* 16 */
#define CROP_Y       ((GB_SCREEN_H - THUMBY_H) / 2)  /* 8 */

/* RGB565 palette lookup — 4 shades */
static uint16_t palette_rgb565[4];

/* Preset palettes (RGB565 values, darkest to lightest indexed 0-3) */
/* Note: GB shade 0 = lightest, shade 3 = darkest */
static const uint16_t palettes[GB_PALETTE_COUNT][4] = {
    /* Classic GB green: lightest (#9BBC0F) to darkest (#0F380F) */
    { 0x9DE1, 0x8D60, 0x3300, 0x0A00 },
    /* Grayscale: white to black */
    { 0xFFFF, 0xAD55, 0x52AA, 0x0000 },
    /* GB Pocket: light to dark */
    { 0xE79C, 0xB596, 0x6B4D, 0x2104 },
};

/* --- Peanut-GB callbacks --- */

static uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < rom_len)
        return rom_ptr[addr];
    return 0xFF;
}

static uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < sizeof(cart_ram))
        return cart_ram[addr];
    return 0xFF;
}

static void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    if (addr < sizeof(cart_ram))
        cart_ram[addr] = val;
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    /* Silently ignore errors to keep running */
    (void)gb;
    (void)err;
    (void)addr;
}

static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    /* Skip lines outside the center crop window */
    if (line < CROP_Y || line >= CROP_Y + THUMBY_H)
        return;

    uint16_t *dst = active_screen_buffer + (line - CROP_Y) * THUMBY_W;
    for (int x = 0; x < THUMBY_W; x++) {
        dst[x] = palette_rgb565[pixels[x + CROP_X] & 0x03];
    }
}

/* --- Public API --- */

int gb_emu_init(uint8_t *rom_data, size_t rom_size) {
    rom_ptr = rom_data;
    rom_len = rom_size;
    memset(cart_ram, 0, sizeof(cart_ram));

    /* Set default palette (classic green) */
    gb_emu_set_palette(GB_PALETTE_GREEN);

    enum gb_init_error_e ret = gb_init(&gb, gb_rom_read_cb, gb_cart_ram_read_cb,
                                        gb_cart_ram_write_cb, gb_error_cb, NULL);
    if (ret != GB_INIT_NO_ERROR)
        return -(int)ret;

    gb_init_lcd(&gb, lcd_draw_line_cb);
    initialized = 1;
    return 0;
}

void gb_emu_run_frame(void) {
    if (initialized)
        gb_run_frame(&gb);
}

void gb_emu_set_buttons(uint8_t buttons) {
    if (initialized)
        gb.direct.joypad = ~buttons;  /* Peanut-GB uses active-low: 0=pressed, 1=released */
}

void gb_emu_reset(void) {
    if (initialized)
        gb_reset(&gb);
}

const char *gb_emu_get_rom_name(char *buf) {
    if (!initialized) {
        buf[0] = '\0';
        return buf;
    }
    return gb_get_rom_name(&gb, buf);
}

void gb_emu_set_palette(int palette_index) {
    if (palette_index < 0 || palette_index >= GB_PALETTE_COUNT)
        palette_index = GB_PALETTE_GREEN;
    memcpy(palette_rgb565, palettes[palette_index], sizeof(palette_rgb565));
}

uint8_t *gb_emu_get_cart_ram(size_t *size_out) {
    size_t sz = gb_emu_get_save_size();
    if (size_out) *size_out = sz;
    return cart_ram;
}

void gb_emu_set_cart_ram(const uint8_t *data, size_t size) {
    if (size > sizeof(cart_ram))
        size = sizeof(cart_ram);
    memcpy(cart_ram, data, size);
}

size_t gb_emu_get_save_size(void) {
    if (!initialized)
        return 0;
    size_t ram_size = 0;
    gb_get_save_size_s(&gb, &ram_size);
    return ram_size;
}
