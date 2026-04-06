/*
 * doom_core.c — Platform glue for DOOM on Thumby Color
 *
 * Implements the doomgeneric portability functions and provides
 * init/deinit/run_loop for integration with the MicroPython engine.
 */

#include "doom_core.h"
#include "doom_cache.h"
#include "doomgeneric/doomgeneric.h"
#include "doomgeneric/doomkeys.h"
#include "doomgeneric/i_video.h"

#include "py/runtime.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

#ifdef __arm__
#include "pico/stdlib.h"
#include "pico/time.h"
#else
#include <time.h>
#include <signal.h>
#include <execinfo.h>
#endif

/* From i_system.c — longjmp error recovery */
extern jmp_buf doom_error_jmp;
extern int doom_error_active;
extern char doom_error_msg[512];

/* Engine externs */
extern uint16_t *active_screen_buffer;
extern bool engine_tick(void);

/* Button access — include the actual engine header */
#include "engine_io_buttons.h"

extern bool button_is_pressed(button_class_obj_t *button);
extern bool button_is_just_pressed(button_class_obj_t *button);

/* Timing — we use clock_gettime directly instead of engine tick timer */

/* WAD file object for ARM MicroPython stream I/O */
mp_obj_t doom_wad_file_obj = MP_OBJ_NULL;
size_t doom_wad_file_size = 0;

/* Display constants */
#define THUMBY_W 128
#define THUMBY_H 128

/* ---- Key queue for DG_GetKey ---- */
#define KEY_QUEUE_SIZE 32
static struct {
    int pressed;
    unsigned char key;
} key_queue[KEY_QUEUE_SIZE];
static int key_queue_head = 0;
static int key_queue_tail = 0;

static void key_queue_push(int pressed, unsigned char key) {
    int next = (key_queue_head + 1) % KEY_QUEUE_SIZE;
    if (next == key_queue_tail) return; /* full */
    key_queue[key_queue_head].pressed = pressed;
    key_queue[key_queue_head].key = key;
    key_queue_head = next;
}

/* Previous button states for edge detection */
static uint16_t prev_buttons = 0;

static void poll_buttons(void) {
    uint16_t cur = 0;
    if (button_is_pressed(&BUTTON_DPAD_UP))    cur |= (1 << 0);
    if (button_is_pressed(&BUTTON_DPAD_DOWN))  cur |= (1 << 1);
    if (button_is_pressed(&BUTTON_DPAD_LEFT))  cur |= (1 << 2);
    if (button_is_pressed(&BUTTON_DPAD_RIGHT)) cur |= (1 << 3);
    if (button_is_pressed(&BUTTON_A))          cur |= (1 << 4);
    if (button_is_pressed(&BUTTON_B))          cur |= (1 << 5);
    if (button_is_pressed(&BUTTON_BUMPER_LEFT))  cur |= (1 << 6);
    if (button_is_pressed(&BUTTON_BUMPER_RIGHT)) cur |= (1 << 7);

    /* Button→DOOM key mapping:
     * D-pad up/down    = forward/back
     * D-pad left/right = turn left/right
     * A                = fire + enter (fire in game, enter in menus)
     * B                = use + escape (use/open in game, back in menus)
     * LB               = strafe modifier
     * RB               = weapon cycle
     */
    static const struct { uint16_t mask; unsigned char key; } mapping[] = {
        { 1 << 0, KEY_UPARROW },
        { 1 << 1, KEY_DOWNARROW },
        { 1 << 2, KEY_LEFTARROW },
        { 1 << 3, KEY_RIGHTARROW },
        { 1 << 4, KEY_FIRE },
        { 1 << 4, KEY_ENTER },     /* A also sends Enter for menus */
        { 1 << 5, KEY_USE },
        { 1 << 5, KEY_ESCAPE },    /* B also sends Escape for menus */
        { 1 << 6, KEY_STRAFE_L },
        { 1 << 7, '/' },  /* next weapon */
    };

    uint16_t changed = cur ^ prev_buttons;
    for (int i = 0; i < 10; i++) {
        if (changed & mapping[i].mask) {
            int pressed = (cur & mapping[i].mask) ? 1 : 0;
            key_queue_push(pressed, mapping[i].key);
        }
    }
    prev_buttons = cur;
}

/* ---- Framebuffer: 128x128 RGBA8888 → RGB565 (Unix emulator only) ---- */
#ifndef DOOM_THUMBY
static void blit_to_screen(void) {
    if (!DG_ScreenBuffer || !active_screen_buffer) return;

    uint32_t *src = (uint32_t *)DG_ScreenBuffer;
    uint16_t *dst = active_screen_buffer;
    int count = THUMBY_W * THUMBY_H;

    for (int i = 0; i < count; i++) {
        uint32_t rgba = src[i];
        dst[i] = (uint16_t)(
            (((rgba >> 16) & 0xF8) << 8) |
            (((rgba >>  8) & 0xFC) << 3) |
            (((rgba >>  0) & 0xF8) >> 3)
        );
    }
}
#endif /* !DOOM_THUMBY */

/* ---- Loading progress bar ---- */
extern void engine_display_send(void);

void doom_loading_progress(int percent) {
    if (!active_screen_buffer) return;
    if (percent > 100) percent = 100;

    /* Black background */
    for (int i = 0; i < THUMBY_W * THUMBY_H; i++)
        active_screen_buffer[i] = 0x0000;

    /* White bar outline at y=60..67, x=14..113 */
    int bx = 14, by = 60, bw = 100, bh = 8;
    for (int x = bx; x < bx+bw; x++) {
        active_screen_buffer[by * THUMBY_W + x] = 0xFFFF;
        active_screen_buffer[(by+bh-1) * THUMBY_W + x] = 0xFFFF;
    }
    for (int y = by; y < by+bh; y++) {
        active_screen_buffer[y * THUMBY_W + bx] = 0xFFFF;
        active_screen_buffer[y * THUMBY_W + bx+bw-1] = 0xFFFF;
    }

    /* Red fill */
    int fill = (bw - 2) * percent / 100;
    for (int y = by+1; y < by+bh-1; y++)
        for (int x = bx+1; x < bx+1+fill; x++)
            active_screen_buffer[y * THUMBY_W + x] = 0xF800;

    /* Push to display — direct hardware on device, engine_tick on emulator */
#ifdef __arm__
    engine_display_send();
#else
    engine_tick();
#endif
}

/* ---- Tick tracking ---- */
static uint32_t doom_start_ticks = 0;
static int initialized = 0;
static char wad_path_buf[256];

/* ---- doomgeneric platform functions ---- */

#ifndef __arm__
static void doom_sigsegv_handler(int sig) {
    void *bt[20];
    int count = backtrace(bt, 20);
    fprintf(stderr, "\n=== DOOM SIGSEGV ===\n");
    backtrace_symbols_fd(bt, count, 2);
    fprintf(stderr, "=== END BACKTRACE ===\n");
    fflush(stderr);
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}
#endif

void DG_Init(void) {
    /* Nothing extra needed — engine already initialized */
}

void DG_DrawFrame(void) {
#ifndef DOOM_THUMBY
    blit_to_screen();
#endif
    /* Thumby: I_FinishUpdate writes directly to active_screen_buffer */
}

void DG_SleepMs(uint32_t ms) {
#ifdef __arm__
    /* Device: short sleep to let time advance, then yield to engine */
    if (ms > 0) {
        sleep_ms(ms);
    }
#else
    if (ms > 0) {
        struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
        nanosleep(&ts, NULL);
    }
#endif
}

static uint32_t doom_get_ms(void) {
#ifdef __arm__
    return to_ms_since_boot(get_absolute_time());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

uint32_t DG_GetTicksMs(void) {
    return doom_get_ms() - doom_start_ticks;
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (key_queue_tail == key_queue_head) return 0;
    *pressed = key_queue[key_queue_tail].pressed;
    *key = key_queue[key_queue_tail].key;
    key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

/* ---- Public API ---- */

int doom_core_init(const char *wad_path) {
#ifndef __arm__
    signal(SIGSEGV, doom_sigsegv_handler);
#endif
    /* Save path for reference */
    strncpy(wad_path_buf, wad_path, sizeof(wad_path_buf) - 1);
    wad_path_buf[sizeof(wad_path_buf) - 1] = '\0';

    doom_start_ticks = doom_get_ms();
    key_queue_head = key_queue_tail = 0;
    prev_buttons = 0;
    initialized = 0;

    /* Set up error recovery — I_Error will longjmp here instead of exit() */
    doom_error_active = 1;
    doom_error_msg[0] = '\0';
    if (setjmp(doom_error_jmp) != 0) {
        /* I_Error was called — return error */
        doom_error_active = 0;
        fprintf(stderr, "DOOM I_Error: %s\n", doom_error_msg);
        return -1;
    }

    /* Show loading screen (device only — emulator init is instant) */
#ifdef __arm__
    doom_loading_progress(5);
#endif

    /* Build argv for doomgeneric — must be static since myargv persists */
    static char *argv[] = { "doom", "-iwad", NULL, "-nosound", NULL };
    argv[2] = wad_path_buf;
    doomgeneric_Create(4, argv);

    doom_error_active = 0;
    initialized = 1;
    return 0;
}

int doom_core_run_loop(void) {
    if (!initialized) return 0;

    volatile int frames = 0;

    /* Set up error recovery for I_Error during gameplay */
    doom_error_active = 1;
    doom_error_msg[0] = '\0';
    if (setjmp(doom_error_jmp) != 0) {
        doom_error_active = 0;
        fprintf(stderr, "DOOM I_Error during run_loop: %s\n", doom_error_msg);
        return frames;
    }

#ifndef __arm__
    /* Install our SIGSEGV handler to get a backtrace (Unix only) */
    signal(SIGSEGV, doom_sigsegv_handler);
#endif

    /* Wait for MENU to be fully released */
    while (1) {
        if (engine_tick() && !button_is_pressed(&BUTTON_MENU))
            break;
    }

    while (1) {
        if (!engine_tick())
            continue;

        /* Check MENU for exit */
        if (button_is_just_pressed(&BUTTON_MENU))
            break;

        /* Poll buttons and push key events */
        poll_buttons();

        /* Run one DOOM tick (processes input, updates game, renders) */
        doomgeneric_Tick();

        frames++;

        /* Handle MicroPython interrupts (Ctrl-C etc) */
        mp_handle_pending(false);
        if (MP_STATE_THREAD(mp_pending_exception) != MP_OBJ_NULL)
            break;
    }

    doom_error_active = 0;
    return frames;
}

void doom_core_set_wad_file(void *file_obj, size_t size) {
    doom_wad_file_obj = (mp_obj_t)file_obj;
    doom_wad_file_size = size;
}

void doom_core_deinit(void) {
    initialized = 0;
    key_queue_head = key_queue_tail = 0;
    prev_buttons = 0;
    doom_wad_file_obj = MP_OBJ_NULL;
    doom_wad_file_size = 0;
}

/* ---- Audio stub (no audio in milestone 1) ---- */

float doom_get_audio_sample(void) {
    return 0.0f;
}
