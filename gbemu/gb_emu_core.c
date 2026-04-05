#define GB_EMU_CORE_IMPL
#include "gb_emu_core.h"
#include "minigb_apu.h"
#include <string.h>
#include <stdio.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "io/engine_io_buttons.h"

/* Engine's active framebuffer — defined in engine_display_common.c */
extern uint16_t *active_screen_buffer;

/* GB state */
static struct gb_s gb;
static uint8_t *rom_ptr = NULL;
static size_t rom_len = 0;
static uint8_t cart_ram[0x8000]; /* 32KB max cart RAM */
static int initialized = 0;

/* File-based ROM reading: 8-bank cache (128KB) for large ROMs */
#define ROM_BANK_CACHE_SIZE 0x4000  /* 16KB per bank (GB ROM bank size) */
#define ROM_BANK_COUNT 8
static uint8_t rom_bank_cache[ROM_BANK_COUNT][ROM_BANK_CACHE_SIZE];
static int32_t rom_bank_ids[ROM_BANK_COUNT];  /* which bank is cached, -1 = empty */
static uint8_t rom_bank_age[ROM_BANK_COUNT];  /* for LRU eviction */
static uint8_t rom_bank_age_counter = 0;
static mp_obj_t rom_file_obj = MP_OBJ_NULL;  /* MP_OBJ_NULL = in-memory mode */

/* Display crop offsets: center 128x128 in 160x144 */
#define GB_SCREEN_W  160
#define GB_SCREEN_H  144
#define THUMBY_W     128
#define THUMBY_H     128
#define CROP_X_MAX   (GB_SCREEN_W - THUMBY_W)  /* 32 */
#define CROP_Y_MAX   (GB_SCREEN_H - THUMBY_H)  /* 16 */

/* Mutable crop offsets — adjustable at runtime */
static int crop_x = CROP_X_MAX / 2;  /* default center: 16 */
static int crop_y = CROP_Y_MAX / 2;  /* default center: 8 */

/* RGB565 palette lookup — 4 shades */
static uint16_t palette_rgb565[4];

/* Preset palettes (RGB565 values, shade 0=lightest to shade 3=darkest) */
static const uint16_t palettes[GB_PALETTE_COUNT][4] = {
    /* Classic GB green */
    { 0x9DE1, 0x8D60, 0x3300, 0x0A00 },
    /* Grayscale */
    { 0xFFFF, 0xAD55, 0x52AA, 0x0000 },
    /* GB Pocket */
    { 0xE79C, 0xB596, 0x6B4D, 0x2104 },
    /* Cream (warm sepia) */
    { 0xFFF5, 0xDECA, 0x9C60, 0x4200 },
    /* Blue */
    { 0xDF9F, 0x5D5F, 0x2A5E, 0x0010 },
    /* Red */
    { 0xFFFF, 0xFBCA, 0xC180, 0x6000 },
};

/* --- Audio state --- */
static struct minigb_apu_ctx apu_ctx;
/* AUDIO_SAMPLES = (22050 / 59.7275) = 369, stereo = 738 */
#define GB_APU_FRAME_SAMPLES 740
static int16_t apu_frame_buf[GB_APU_FRAME_SAMPLES];

/* Ring buffer for mono audio samples consumed by engine */
#define GB_AUDIO_RING_SIZE 4096
static int16_t audio_ring[GB_AUDIO_RING_SIZE];
static volatile uint16_t audio_ring_write = 0;
static volatile uint16_t audio_ring_read = 0;
static int audio_enabled = 1;

/* --- Peanut-GB callbacks --- */

static uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr >= rom_len)
        return 0xFF;

    /* In-memory mode: direct access */
    if (rom_ptr != NULL)
        return rom_ptr[addr];

    /* File-based mode: read from bank cache */
    int32_t bank_id = (int32_t)(addr / ROM_BANK_CACHE_SIZE);
    uint32_t bank_offset = addr % ROM_BANK_CACHE_SIZE;

    /* Check if bank is cached */
    for (int i = 0; i < ROM_BANK_COUNT; i++) {
        if (rom_bank_ids[i] == bank_id) {
            rom_bank_age[i] = ++rom_bank_age_counter;
            return rom_bank_cache[i][bank_offset];
        }
    }

    /* Cache miss: find LRU slot (oldest age) */
    int slot = 0;
    uint8_t oldest = rom_bank_age[0];
    for (int i = 1; i < ROM_BANK_COUNT; i++) {
        /* Prefer empty slots first */
        if (rom_bank_ids[i] < 0) { slot = i; break; }
        if ((uint8_t)(rom_bank_age[i] - oldest) > 127) {
            /* rom_bank_age[i] is older (wrapping comparison) */
            oldest = rom_bank_age[i];
            slot = i;
        }
    }

    rom_bank_ids[slot] = bank_id;
    rom_bank_age[slot] = ++rom_bank_age_counter;
    uint32_t file_offset = bank_id * ROM_BANK_CACHE_SIZE;

    /* Seek to bank offset using MicroPython stream API */
    struct mp_stream_seek_t seek_s = { .offset = file_offset, .whence = 0 };
    mp_obj_t stream = rom_file_obj;
    const mp_stream_p_t *stream_p = mp_get_stream(stream);
    stream_p->ioctl(stream, MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, NULL);

    /* Read bank data */
    size_t to_read = ROM_BANK_CACHE_SIZE;
    if (file_offset + to_read > rom_len)
        to_read = rom_len - file_offset;
    memset(rom_bank_cache[slot], 0xFF, ROM_BANK_CACHE_SIZE);
    int errcode = 0;
    stream_p->read(stream, rom_bank_cache[slot], to_read, &errcode);

    return rom_bank_cache[slot][bank_offset];
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
    (void)gb; (void)err; (void)addr;
}

static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    if (line < crop_y || line >= crop_y + THUMBY_H)
        return;

    uint16_t *dst = active_screen_buffer + (line - crop_y) * THUMBY_W;
    for (int x = 0; x < THUMBY_W; x++) {
        dst[x] = palette_rgb565[pixels[x + crop_x] & 0x03];
    }
}

/* Peanut-GB audio callbacks — called when GB code reads/writes APU registers */
void audio_write(uint16_t addr, uint8_t val) {
    minigb_apu_audio_write(&apu_ctx, addr, val);
}

uint8_t audio_read(uint16_t addr) {
    return minigb_apu_audio_read(&apu_ctx, addr);
}

/* --- Public API --- */

static void gb_emu_common_init(void) {
    memset(cart_ram, 0, sizeof(cart_ram));
    gb_emu_set_palette(GB_PALETTE_GREEN);
    for (int i = 0; i < ROM_BANK_COUNT; i++) {
        rom_bank_ids[i] = -1;
        rom_bank_age[i] = 0;
    }
    rom_bank_age_counter = 0;
}

int gb_emu_init(uint8_t *rom_data, size_t rom_size) {
    rom_file_obj = MP_OBJ_NULL;
    rom_ptr = rom_data;
    rom_len = rom_size;
    gb_emu_common_init();

    enum gb_init_error_e ret = gb_init(&gb, gb_rom_read_cb, gb_cart_ram_read_cb,
                                        gb_cart_ram_write_cb, gb_error_cb, NULL);
    if (ret != GB_INIT_NO_ERROR)
        return -(int)ret;

    gb_init_lcd(&gb, lcd_draw_line_cb);
    minigb_apu_audio_init(&apu_ctx);
    audio_ring_write = 0;
    audio_ring_read = 0;
    audio_enabled = 1;
    initialized = 1;
    return 0;
}

int gb_emu_init_file(void *file_obj, size_t file_size) {
    rom_file_obj = MP_OBJ_NULL;
    rom_ptr = NULL;
    rom_len = file_size;

    if (rom_len < 0x150)
        return -101;

    rom_file_obj = (mp_obj_t)file_obj;
    gb_emu_common_init();

    /* Pre-cache bank 0 (contains header, always needed) */
    rom_bank_ids[0] = 0;
    size_t to_read = rom_len < ROM_BANK_CACHE_SIZE ? rom_len : ROM_BANK_CACHE_SIZE;
    memset(rom_bank_cache[0], 0xFF, ROM_BANK_CACHE_SIZE);

    const mp_stream_p_t *stream_p = mp_get_stream(rom_file_obj);
    struct mp_stream_seek_t seek_s = { .offset = 0, .whence = 0 };
    stream_p->ioctl(rom_file_obj, MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, NULL);
    int errcode = 0;
    stream_p->read(rom_file_obj, rom_bank_cache[0], to_read, &errcode);

    enum gb_init_error_e ret = gb_init(&gb, gb_rom_read_cb, gb_cart_ram_read_cb,
                                        gb_cart_ram_write_cb, gb_error_cb, NULL);
    if (ret != GB_INIT_NO_ERROR) {
        rom_file_obj = MP_OBJ_NULL;
        return -(int)ret;
    }

    gb_init_lcd(&gb, lcd_draw_line_cb);
    minigb_apu_audio_init(&apu_ctx);
    audio_ring_write = 0;
    audio_ring_read = 0;
    audio_enabled = 1;
    initialized = 1;
    return 0;
}

void gb_emu_run_frame(void) {
    if (!initialized) return;

    gb_run_frame(&gb);

    /* Generate audio samples for this frame */
    if (audio_enabled) {
        minigb_apu_audio_callback(&apu_ctx, apu_frame_buf);

        /* Mix stereo to mono and push into ring buffer.
         * AUDIO_SAMPLES = 22050/59.7275 ≈ 369 samples per frame. */
        unsigned num_samples = (unsigned)(AUDIO_SAMPLE_RATE / VERTICAL_SYNC);
        for (unsigned i = 0; i < num_samples; i++) {
            int32_t left = apu_frame_buf[i * 2];
            int32_t right = apu_frame_buf[i * 2 + 1];
            int16_t mono = (int16_t)((left + right) / 2);

            uint16_t next_w = (audio_ring_write + 1) & (GB_AUDIO_RING_SIZE - 1);
            if (next_w != audio_ring_read) {
                audio_ring[audio_ring_write] = mono;
                audio_ring_write = next_w;
            }
            /* else: buffer full, drop sample */
        }
    }
}

/* Called by engine's audio callback at 22050Hz */
static float last_audio_sample = 0.0f;

float gb_emu_get_audio_sample(void) {
    if (!audio_enabled)
        return 0.0f;

    if (audio_ring_read == audio_ring_write) {
        /* Buffer underrun: hold last sample to avoid clicks */
        last_audio_sample *= 0.998f;  /* gentle fade to avoid DC offset */
        return last_audio_sample;
    }

    int16_t sample = audio_ring[audio_ring_read];
    audio_ring_read = (audio_ring_read + 1) & (GB_AUDIO_RING_SIZE - 1);

    last_audio_sample = (float)sample / 32767.0f;
    return last_audio_sample;
}

void gb_emu_set_audio_enabled(int enabled) {
    audio_enabled = enabled;
    if (!enabled) {
        audio_ring_write = 0;
        audio_ring_read = 0;
    }
}

void gb_emu_set_crop(int x, int y) {
    if (x < 0) x = 0;
    if (x > CROP_X_MAX) x = CROP_X_MAX;
    if (y < 0) y = 0;
    if (y > CROP_Y_MAX) y = CROP_Y_MAX;
    crop_x = x;
    crop_y = y;
}

int gb_emu_get_crop_x(void) { return crop_x; }
int gb_emu_get_crop_y(void) { return crop_y; }

void gb_emu_set_buttons(uint8_t buttons) {
    if (initialized)
        gb.direct.joypad = ~buttons;
}

static int show_fps = 0;

void gb_emu_set_show_fps(int show) { show_fps = show; }

/* Tiny 3x5 font for FPS display — digits 0-9 */
static const uint8_t digit_font[10][5] = {
    {0x7,0x5,0x5,0x5,0x7}, /* 0 */
    {0x2,0x6,0x2,0x2,0x7}, /* 1 */
    {0x7,0x1,0x7,0x4,0x7}, /* 2 */
    {0x7,0x1,0x7,0x1,0x7}, /* 3 */
    {0x5,0x5,0x7,0x1,0x1}, /* 4 */
    {0x7,0x4,0x7,0x1,0x7}, /* 5 */
    {0x7,0x4,0x7,0x5,0x7}, /* 6 */
    {0x7,0x1,0x1,0x1,0x1}, /* 7 */
    {0x7,0x5,0x7,0x5,0x7}, /* 8 */
    {0x7,0x5,0x7,0x1,0x7}, /* 9 */
};

static void draw_fps(int fps) {
    if (!active_screen_buffer || !show_fps) return;
    /* Draw up to 3 digits at top-right corner */
    char buf[4];
    int len = 0;
    if (fps >= 100) buf[len++] = (fps / 100) % 10;
    if (fps >= 10) buf[len++] = (fps / 10) % 10;
    buf[len++] = fps % 10;

    int start_x = THUMBY_W - len * 4 - 1;
    for (int d = 0; d < len; d++) {
        int digit = buf[d];
        int dx = start_x + d * 4;
        for (int row = 0; row < 5; row++) {
            uint8_t bits = digit_font[digit][row];
            for (int col = 0; col < 3; col++) {
                if (bits & (4 >> col)) {
                    active_screen_buffer[(row + 1) * THUMBY_W + dx + col] = 0xFFFF;
                }
            }
        }
    }
}

/* High-performance all-C frame loop. Runs until MENU is pressed.
 * Returns number of frames run. Eliminates Python interpreter overhead. */
int gb_emu_run_loop(void) {
    if (!initialized) return 0;

    extern bool engine_tick(void);
    extern button_class_obj_t BUTTON_A, BUTTON_B;
    extern button_class_obj_t BUTTON_BUMPER_LEFT, BUTTON_BUMPER_RIGHT;
    extern button_class_obj_t BUTTON_DPAD_UP, BUTTON_DPAD_DOWN;
    extern button_class_obj_t BUTTON_DPAD_LEFT, BUTTON_DPAD_RIGHT;
    extern button_class_obj_t BUTTON_MENU;

    int frames = 0;

    while (1) {
        if (!engine_tick())
            continue;

        /* Check MENU for pause — must check before overwriting joypad */
        if (button_is_just_pressed(&BUTTON_MENU))
            break;

        /* Read buttons directly from engine_io button objects */
        uint8_t buttons = 0;

        /* Check if LB is held for viewport panning */
        if (button_is_pressed(&BUTTON_BUMPER_LEFT)) {
            if (button_is_pressed(&BUTTON_DPAD_LEFT))  gb_emu_set_crop(crop_x - 2, crop_y);
            if (button_is_pressed(&BUTTON_DPAD_RIGHT)) gb_emu_set_crop(crop_x + 2, crop_y);
            if (button_is_pressed(&BUTTON_DPAD_UP))    gb_emu_set_crop(crop_x, crop_y - 2);
            if (button_is_pressed(&BUTTON_DPAD_DOWN))  gb_emu_set_crop(crop_x, crop_y + 2);
            /* Only pass A, B, RB to GB while panning */
            if (button_is_pressed(&BUTTON_A)) buttons |= JOYPAD_A;
            if (button_is_pressed(&BUTTON_B)) buttons |= JOYPAD_B;
            if (button_is_pressed(&BUTTON_BUMPER_RIGHT)) buttons |= JOYPAD_START;
        } else {
            if (button_is_pressed(&BUTTON_A)) buttons |= JOYPAD_A;
            if (button_is_pressed(&BUTTON_B)) buttons |= JOYPAD_B;
            if (button_is_pressed(&BUTTON_BUMPER_LEFT))  buttons |= JOYPAD_SELECT;
            if (button_is_pressed(&BUTTON_BUMPER_RIGHT)) buttons |= JOYPAD_START;
            if (button_is_pressed(&BUTTON_DPAD_UP))    buttons |= JOYPAD_UP;
            if (button_is_pressed(&BUTTON_DPAD_DOWN))  buttons |= JOYPAD_DOWN;
            if (button_is_pressed(&BUTTON_DPAD_LEFT))  buttons |= JOYPAD_LEFT;
            if (button_is_pressed(&BUTTON_DPAD_RIGHT)) buttons |= JOYPAD_RIGHT;
        }

        gb.direct.joypad = ~buttons;
        gb_emu_run_frame();

        /* Draw FPS overlay after GB frame renders */
        if (show_fps) {
            extern uint32_t engine_fps_time_at_last_tick_ms;
            extern uint32_t engine_fps_time_at_before_last_tick_ms;
            if (engine_fps_time_at_before_last_tick_ms != 0xFFFFFFFF) {
                uint32_t dt = engine_fps_time_at_last_tick_ms - engine_fps_time_at_before_last_tick_ms;
                if (dt > 0) draw_fps(1000 / dt);
            }
        }

        frames++;

        /* Handle MicroPython interrupts (Ctrl-C etc) */
        mp_handle_pending(false);
        if (MP_STATE_THREAD(mp_pending_exception) != MP_OBJ_NULL)
            break;
    }

    return frames;
}

void gb_emu_reset(void) {
    if (initialized) {
        gb_reset(&gb);
        minigb_apu_audio_init(&apu_ctx);
        audio_ring_write = 0;
        audio_ring_read = 0;
    }
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

/* --- Save states --- */

size_t gb_emu_get_state_size(void) {
    return sizeof(struct gb_s) + sizeof(struct minigb_apu_ctx);
}

int gb_emu_save_state(uint8_t *buf, size_t buf_size) {
    if (!initialized) return -1;
    size_t needed = gb_emu_get_state_size();
    if (buf_size < needed) return -2;
    memcpy(buf, &gb, sizeof(struct gb_s));
    memcpy(buf + sizeof(struct gb_s), &apu_ctx, sizeof(struct minigb_apu_ctx));
    return 0;
}

int gb_emu_load_state(const uint8_t *buf, size_t buf_size) {
    if (!initialized) return -1;
    size_t needed = gb_emu_get_state_size();
    if (buf_size < needed) return -2;

    /* Restore GB state */
    memcpy(&gb, buf, sizeof(struct gb_s));

    /* Re-set function pointers (not valid from serialized data) */
    gb.gb_rom_read = gb_rom_read_cb;
    gb.gb_cart_ram_read = gb_cart_ram_read_cb;
    gb.gb_cart_ram_write = gb_cart_ram_write_cb;
    gb.gb_error = gb_error_cb;
    gb.display.lcd_draw_line = lcd_draw_line_cb;
    gb.gb_serial_tx = NULL;
    gb.gb_serial_rx = NULL;
    gb.gb_bootrom_read = NULL;
    gb.direct.priv = NULL;

    /* Restore APU state */
    memcpy(&apu_ctx, buf + sizeof(struct gb_s), sizeof(struct minigb_apu_ctx));

    return 0;
}

size_t gb_emu_get_save_size(void) {
    if (!initialized)
        return 0;
    size_t ram_size = 0;
    gb_get_save_size_s(&gb, &ram_size);
    return ram_size;
}
