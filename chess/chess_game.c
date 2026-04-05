/*
 * chess_game.c — All-C game loop for DeepThumb chess
 *
 * Renders directly to the engine's framebuffer, polls buttons,
 * and drives the Chal chess engine for AI moves.
 */

#include "chess_game.h"
#include "chal.h"
#include "mcu-max.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "io/engine_io_buttons.h"
#include <string.h>

/* Engine types */
#define ENGINE_CHAL   0
#define ENGINE_MCUMAX 1
static const char *engine_names[] = { "CHAL", "MCU-MAX" };

/* Difficulty settings per engine */
static const uint32_t chal_depth[] = { 2, 4, 6, 64 };
static const uint32_t chal_time[]  = { 500, 1500, 3000, 8000 };
static const char *chal_elo[]      = { "~1200", "~1600", "~1900", "~2200" };
static const uint32_t mcumax_nodes[] = { 5000, 20000, 100000, 500000 };
static const uint32_t mcumax_depth[] = { 3, 4, 5, 6 };
static const char *mcumax_elo[]      = { "~1000", "~1400", "~1700", "~1900" };

/* ---- AI search result (filled by blocking search) ---- */
static uint8_t search_result_from;
static uint8_t search_result_to;
static int search_result_valid;
static uint8_t search_result_promo;

/* Engine framebuffer */
extern uint16_t *active_screen_buffer;

/* Engine tick — handles frame timing, display refresh, buffer swap */
extern bool engine_tick(void);

/* Buttons */
extern button_class_obj_t BUTTON_A, BUTTON_B;
extern button_class_obj_t BUTTON_BUMPER_LEFT, BUTTON_BUMPER_RIGHT;
extern button_class_obj_t BUTTON_DPAD_UP, BUTTON_DPAD_DOWN;
extern button_class_obj_t BUTTON_DPAD_LEFT, BUTTON_DPAD_RIGHT;
extern button_class_obj_t BUTTON_MENU;

/* Screen dimensions */
#define SCREEN_W 128
#define SCREEN_H 128
#define TILE_SIZE 16

/* Board colors (RGB565) */
#define COLOR_LIGHT     0xEF3C  /* warm cream */
#define COLOR_DARK      0x8C51  /* brown */
#define COLOR_CURSOR     0x07FF  /* cyan outline */
#define COLOR_SELECTED   0xFFE0  /* yellow highlight */
#define COLOR_VALID_MOVE 0x47E0  /* green dot */
#define COLOR_LAST_FROM  0xBDF7  /* light blue */
#define COLOR_LAST_TO    0xBDF7
#define COLOR_CHECK      0xF800  /* red */
#define COLOR_BG         0x2104  /* dark grey background for status area */
#define COLOR_TEXT_WHITE  0xFFFF
#define COLOR_TEXT_DIM    0x8410

/* Piece sprite layout in chess.bmp (96x32, 6x2 grid of 16x16):
 * Column: Rook=0, Knight=1, Bishop=2, King=3, Queen=4, Pawn=5
 * Row: White=0 (bottom in BMP), Black=1 (top in BMP)
 * Note: BMP is bottom-up, so row 0 is at the bottom of the image. */
#define SPRITE_ROOK   0
#define SPRITE_KNIGHT 1
#define SPRITE_BISHOP 2
#define SPRITE_KING   3
#define SPRITE_QUEEN  4
#define SPRITE_PAWN   5

/* Transparent color in sprites (white — the sprite sheet background) */
#define TRANSPARENT_COLOR 0xFFFF

/* Game states */
typedef enum {
    STATE_TITLE,
    STATE_SETUP,
    STATE_PLAYER_TURN,
    STATE_AI_THINKING,
    STATE_GAME_OVER,
} game_state_t;

/* Game context */
static struct {
    game_state_t state;

    /* Player/AI setup */
    int player_is_white;    /* 1 = player is white, 0 = player is black */
    int difficulty;         /* 0=easy, 1=medium, 2=hard, 3=expert */
    int engine;             /* ENGINE_CHAL or ENGINE_MCUMAX */

    /* Cursor */
    int cursor_file;        /* 0-7 */
    int cursor_rank;        /* 0-7 */

    /* Selection */
    int selected;           /* 1 if a piece is selected */
    int sel_file, sel_rank;

    /* Legal moves for selected piece */
    chal_move_info_t legal_moves[256];
    int legal_move_count;

    /* Last move highlight */
    int has_last_move;
    int last_from_file, last_from_rank;
    int last_to_file, last_to_rank;

    /* Game result */
    int result;  /* 0=ongoing, 1=checkmate, 2=stalemate */
    int winner_is_white;

    /* AI thinking */
    int think_frame;

    /* Evaluation score (centipawns, from white's perspective) */
    int eval_score;

    /* Settings */
    int sound_on;
    int show_eval_bar;

    /* Pause menu */
    int paused;
    int pause_cursor;  /* 0=resume, 1=sound, 2=eval, 3=quit */

    /* Move count for undo tracking */
    int move_count;  /* total half-moves played */

    /* Sprite data */
    uint16_t *sprite_data;
    int sprite_w, sprite_h;
    uint16_t *board_data;
    int board_w, board_h;

    /* Sound resources (MicroPython objects) */
    mp_obj_t snd_move;
    mp_obj_t snd_take;
    mp_obj_t snd_pawn;
} game;

static const char *diff_names[] = { "Easy", "Medium", "Hard", "Expert" };

/* ---- Engine abstraction wrappers ---- */

static const int mcumax_type_to_chal[] = {
    0, CHAL_PAWN, CHAL_PAWN, CHAL_KNIGHT, CHAL_KING, CHAL_BISHOP, CHAL_ROOK, CHAL_QUEEN
};

static int engine_get_piece(int rank, int file) {
    if (game.engine == ENGINE_CHAL) {
        return chal_get_piece(rank, file);
    } else {
        uint8_t sq = (rank << 4) | file;
        mcumax_piece raw = mcumax_get_piece(sq);
        int mtype = raw & 0x7;
        if (mtype == MCUMAX_EMPTY) return 0;
        int is_black = (raw & MCUMAX_BLACK) ? 1 : 0;
        return (is_black << 3) | mcumax_type_to_chal[mtype];
    }
}

static int engine_get_side(void) {
    if (game.engine == ENGINE_CHAL) return chal_get_side();
    return (mcumax_get_current_side() == 0x8) ? 0 : 1;
}

static void engine_new_game(void) {
    if (game.engine == ENGINE_CHAL) chal_new_game();
    else mcumax_init();
}

/* Forward declarations */
static void update_eval(void);
static void draw_eval_bar(void);

/* ---- Sound playback via engine_audio C API ---- */
#include "audio/engine_audio_module.h"
#include "audio/engine_audio_channel.h"
extern volatile mp_obj_t channels[];

static void play_sound(mp_obj_t snd) {
    if (!game.sound_on || snd == mp_const_none) return;
    audio_channel_class_obj_t *ch = (audio_channel_class_obj_t *)channels[0];
    if (ch != NULL) {
        engine_audio_play_on_channel(snd, ch, mp_const_false);
    }
}

static void init_audio(void) {
    /* channels are initialized by engine_audio_setup, nothing extra needed */
}

/* ---- Tiny 4x6 font for status text ---- */
/* ASCII 32-90 (space through Z), 4 pixels wide, 6 pixels tall */
/* Each char is 6 bytes, one per row, bits 3-0 = pixels left to right */
static const uint8_t mini_font[][6] = {
    /* space */ {0x0,0x0,0x0,0x0,0x0,0x0},
    /* ! */     {0x4,0x4,0x4,0x0,0x4,0x0},
    /* " */     {0xA,0xA,0x0,0x0,0x0,0x0},
    /* # */     {0xA,0xF,0xA,0xF,0xA,0x0},
    /* $ */     {0x4,0xE,0xC,0x6,0xE,0x4},
    /* % */     {0x9,0x2,0x4,0x9,0x0,0x0},
    /* & */     {0x4,0xA,0x4,0xA,0x5,0x0},
    /* ' */     {0x4,0x4,0x0,0x0,0x0,0x0},
    /* ( */     {0x2,0x4,0x4,0x4,0x2,0x0},
    /* ) */     {0x4,0x2,0x2,0x2,0x4,0x0},
    /* * */     {0x0,0xA,0x4,0xA,0x0,0x0},
    /* + */     {0x0,0x4,0xE,0x4,0x0,0x0},
    /* , */     {0x0,0x0,0x0,0x4,0x4,0x8},
    /* - */     {0x0,0x0,0xE,0x0,0x0,0x0},
    /* . */     {0x0,0x0,0x0,0x0,0x4,0x0},
    /* / */     {0x1,0x2,0x4,0x8,0x0,0x0},
    /* 0 */     {0x6,0x9,0x9,0x9,0x6,0x0},
    /* 1 */     {0x4,0xC,0x4,0x4,0xE,0x0},
    /* 2 */     {0x6,0x9,0x2,0x4,0xF,0x0},
    /* 3 */     {0xE,0x1,0x6,0x1,0xE,0x0},
    /* 4 */     {0x2,0x6,0xA,0xF,0x2,0x0},
    /* 5 */     {0xF,0x8,0xE,0x1,0xE,0x0},
    /* 6 */     {0x6,0x8,0xE,0x9,0x6,0x0},
    /* 7 */     {0xF,0x1,0x2,0x4,0x4,0x0},
    /* 8 */     {0x6,0x9,0x6,0x9,0x6,0x0},
    /* 9 */     {0x6,0x9,0x7,0x1,0x6,0x0},
    /* : */     {0x0,0x4,0x0,0x4,0x0,0x0},
    /* ; */     {0x0,0x4,0x0,0x4,0x8,0x0},
    /* < */     {0x1,0x2,0x4,0x2,0x1,0x0},
    /* = */     {0x0,0xE,0x0,0xE,0x0,0x0},
    /* > */     {0x4,0x2,0x1,0x2,0x4,0x0},
    /* ? */     {0x6,0x9,0x2,0x0,0x2,0x0},
    /* @ */     {0x6,0x9,0xB,0x8,0x6,0x0},
    /* A */     {0x6,0x9,0xF,0x9,0x9,0x0},
    /* B */     {0xE,0x9,0xE,0x9,0xE,0x0},
    /* C */     {0x7,0x8,0x8,0x8,0x7,0x0},
    /* D */     {0xE,0x9,0x9,0x9,0xE,0x0},
    /* E */     {0xF,0x8,0xE,0x8,0xF,0x0},
    /* F */     {0xF,0x8,0xE,0x8,0x8,0x0},
    /* G */     {0x7,0x8,0xB,0x9,0x7,0x0},
    /* H */     {0x9,0x9,0xF,0x9,0x9,0x0},
    /* I */     {0xE,0x4,0x4,0x4,0xE,0x0},
    /* J */     {0x7,0x1,0x1,0x9,0x6,0x0},
    /* K */     {0x9,0xA,0xC,0xA,0x9,0x0},
    /* L */     {0x8,0x8,0x8,0x8,0xF,0x0},
    /* M */     {0x9,0xF,0xF,0x9,0x9,0x0},
    /* N */     {0x9,0xD,0xB,0x9,0x9,0x0},
    /* O */     {0x6,0x9,0x9,0x9,0x6,0x0},
    /* P */     {0xE,0x9,0xE,0x8,0x8,0x0},
    /* Q */     {0x6,0x9,0x9,0xA,0x5,0x0},
    /* R */     {0xE,0x9,0xE,0xA,0x9,0x0},
    /* S */     {0x7,0x8,0x6,0x1,0xE,0x0},
    /* T */     {0xF,0x4,0x4,0x4,0x4,0x0},
    /* U */     {0x9,0x9,0x9,0x9,0x6,0x0},
    /* V */     {0x9,0x9,0x9,0x6,0x6,0x0},
    /* W */     {0x9,0x9,0xF,0xF,0x9,0x0},
    /* X */     {0x9,0x6,0x6,0x6,0x9,0x0},
    /* Y */     {0x9,0x9,0x7,0x1,0x6,0x0},
    /* Z */     {0xF,0x2,0x4,0x8,0xF,0x0},
};

static void draw_char(int x, int y, char c, uint16_t color) {
    if (c < 32 || c > 90) return;
    int idx = c - 32;
    for (int row = 0; row < 6; row++) {
        if (y + row < 0 || y + row >= SCREEN_H) continue;
        uint8_t bits = mini_font[idx][row];
        for (int col = 0; col < 4; col++) {
            if (x + col < 0 || x + col >= SCREEN_W) continue;
            if (bits & (8 >> col)) {
                active_screen_buffer[(y + row) * SCREEN_W + x + col] = color;
            }
        }
    }
}

static void draw_text(int x, int y, const char *str, uint16_t color) {
    while (*str) {
        char c = *str++;
        if (c >= 'a' && c <= 'z') c -= 32;  /* uppercase only in our font */
        draw_char(x, y, c, color);
        x += 5;
    }
}

/* ---- Drawing helpers ---- */

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int row = y; row < y + h && row < SCREEN_H; row++) {
        if (row < 0) continue;
        for (int col = x; col < x + w && col < SCREEN_W; col++) {
            if (col < 0) continue;
            active_screen_buffer[row * SCREEN_W + col] = color;
        }
    }
}

static void draw_rect_outline(int x, int y, int w, int h, uint16_t color) {
    for (int i = x; i < x + w; i++) {
        if (i >= 0 && i < SCREEN_W) {
            if (y >= 0 && y < SCREEN_H) active_screen_buffer[y * SCREEN_W + i] = color;
            if (y+h-1 >= 0 && y+h-1 < SCREEN_H) active_screen_buffer[(y+h-1) * SCREEN_W + i] = color;
        }
    }
    for (int i = y; i < y + h; i++) {
        if (i >= 0 && i < SCREEN_H) {
            if (x >= 0 && x < SCREEN_W) active_screen_buffer[i * SCREEN_W + x] = color;
            if (x+w-1 >= 0 && x+w-1 < SCREEN_W) active_screen_buffer[i * SCREEN_W + x+w-1] = color;
        }
    }
}

/* Blit a 16x16 sprite from the sprite sheet.
 * sprite_col: 0-5 (piece type), sprite_row: 0-1 (color).
 * BMP is bottom-up: row 0 = bottom of image = white pieces,
 * row 1 = top of image = black pieces.
 * In the raw pixel data (after BMP loading by TextureResource),
 * the engine flips it to top-down, so row 0 = top = black, row 1 = bottom = white. */
static void blit_sprite(int screen_x, int screen_y, int sprite_col, int sprite_row) {
    if (!game.sprite_data) return;

    /* TextureResource loads BMP as top-down RGB565.
     * In the 96x32 sheet: top 16 rows = black pieces (row 0 in our indexing),
     * bottom 16 rows = white pieces (row 1).
     * But BMP is bottom-up, and TextureResource flips it.
     * Original BMP: bottom row 0 = white, top row 1 = black.
     * After flip: top = row 1 (black), bottom = row 0 (white).
     * So: sprite_row 0 (white) is at y_offset = 16, sprite_row 1 (black) is at y_offset = 0. */
    int src_x = sprite_col * TILE_SIZE;
    int src_y = sprite_row ? 0 : TILE_SIZE;  /* black=top(0), white=bottom(16) */

    for (int row = 0; row < TILE_SIZE; row++) {
        int sy = src_y + row;
        int dy = screen_y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < TILE_SIZE; col++) {
            int sx = src_x + col;
            int dx = screen_x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            uint16_t pixel = game.sprite_data[sy * game.sprite_w + sx];
            if (pixel != TRANSPARENT_COLOR) {
                active_screen_buffer[dy * SCREEN_W + dx] = pixel;
            }
        }
    }
}

/* Blit a board tile (light=0, dark=1) */
static void blit_board_tile(int screen_x, int screen_y, int tile_idx) {
    if (!game.board_data) {
        /* Fallback: use solid colors */
        fill_rect(screen_x, screen_y, TILE_SIZE, TILE_SIZE,
                  tile_idx ? COLOR_DARK : COLOR_LIGHT);
        return;
    }

    int src_x = tile_idx * TILE_SIZE;
    for (int row = 0; row < TILE_SIZE; row++) {
        int dy = screen_y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < TILE_SIZE; col++) {
            int dx = screen_x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            active_screen_buffer[dy * SCREEN_W + dx] =
                game.board_data[row * game.board_w + src_x + col];
        }
    }
}

/* Get piece for rendering — always live board (no snapshot needed with blocking search) */
static inline uint8_t get_piece_for_render(int file, int rank) {
    return engine_get_piece(rank, file);
}

/* Convert game coordinates (rank 0 = top = rank 8) to Chal 0x88 square
 * (rank 0 = rank 1 = white back rank) */
static inline uint8_t game_to_0x88(int rank, int file) {
    return (uint8_t)((7 - rank) * 16 + file);
}

/* Convert Chal 0x88 square to game rank and file */
static inline void sq88_to_game(uint8_t sq, int *rank, int *file) {
    *rank = 7 - (sq >> 4);
    *file = sq & 7;
}

/* Convert Chal piece to sprite column index.
 * Chal pieces: (color<<3)|type. color: 0=white, 1=black. type: 1=pawn..6=king.
 * Sprite columns: Rook=0, Knight=1, Bishop=2, King=3, Queen=4, Pawn=5 */
static int piece_to_sprite_col(uint8_t piece) {
    int ptype = piece & 0x7;
    switch (ptype) {
        case CHAL_PAWN:   return SPRITE_PAWN;
        case CHAL_KNIGHT: return SPRITE_KNIGHT;
        case CHAL_BISHOP: return SPRITE_BISHOP;
        case CHAL_ROOK:   return SPRITE_ROOK;
        case CHAL_QUEEN:  return SPRITE_QUEEN;
        case CHAL_KING:   return SPRITE_KING;
        default: return -1;
    }
}

/* Is a move in the legal moves list for the selected piece? */
static int is_valid_target(int file, int rank) {
    uint8_t to = (game.engine == ENGINE_CHAL)
        ? game_to_0x88(rank, file)
        : (uint8_t)((rank << 4) | file);
    for (int i = 0; i < game.legal_move_count; i++) {
        if (game.legal_moves[i].to == to)
            return 1;
    }
    return 0;
}

/* ---- Board rendering ---- */

/* Board occupies the full 128x128 screen (8x8 tiles of 16x16).
 * When player is black, the board is flipped. */
static void draw_board(void) {
    int flip = !game.player_is_white;

    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            int display_rank = flip ? (7 - rank) : rank;
            int display_file = flip ? (7 - file) : file;
            int screen_x = display_file * TILE_SIZE;
            int screen_y = display_rank * TILE_SIZE;

            /* Board square color */
            int is_dark = (rank + file) % 2;
            blit_board_tile(screen_x, screen_y, is_dark);

            /* Last move highlight */
            if (game.has_last_move) {
                if ((file == game.last_from_file && rank == game.last_from_rank) ||
                    (file == game.last_to_file && rank == game.last_to_rank)) {
                    /* Tint the square with a semi-transparent overlay */
                    for (int row = 1; row < TILE_SIZE - 1; row++) {
                        for (int col = 1; col < TILE_SIZE - 1; col++) {
                            int px = screen_x + col;
                            int py = screen_y + row;
                            uint16_t orig = active_screen_buffer[py * SCREEN_W + px];
                            /* Blend with yellow: average with 0xFFE0 */
                            uint16_t r = ((orig >> 11) + (0xFFE0 >> 11)) / 2;
                            uint16_t g = (((orig >> 5) & 0x3F) + ((0xFFE0 >> 5) & 0x3F)) / 2;
                            uint16_t b = ((orig & 0x1F) + (0xFFE0 & 0x1F)) / 2;
                            active_screen_buffer[py * SCREEN_W + px] = (r << 11) | (g << 5) | b;
                        }
                    }
                }
            }

            /* Selected piece highlight */
            if (game.selected && file == game.sel_file && rank == game.sel_rank) {
                fill_rect(screen_x + 1, screen_y + 1, TILE_SIZE - 2, TILE_SIZE - 2, COLOR_SELECTED);
            }

            /* Valid move dots */
            if (game.selected && is_valid_target(file, rank)) {
                int p = get_piece_for_render(file, rank);
                if ((p & 0x7) == CHAL_EMPTY) {
                    /* Empty square: small dot in center */
                    fill_rect(screen_x + 6, screen_y + 6, 4, 4, COLOR_VALID_MOVE);
                } else {
                    /* Capture: corner markers */
                    fill_rect(screen_x, screen_y, 3, 3, COLOR_VALID_MOVE);
                    fill_rect(screen_x + 13, screen_y, 3, 3, COLOR_VALID_MOVE);
                    fill_rect(screen_x, screen_y + 13, 3, 3, COLOR_VALID_MOVE);
                    fill_rect(screen_x + 13, screen_y + 13, 3, 3, COLOR_VALID_MOVE);
                }
            }

            /* Draw piece */
            int piece = get_piece_for_render(file, rank);
            if ((piece & 0x7) != CHAL_EMPTY) {
                int col_idx = piece_to_sprite_col(piece);
                int is_black = (piece & (CHAL_BLACK << 3)) != 0;
                if (col_idx >= 0) {
                    blit_sprite(screen_x, screen_y, col_idx, is_black ? 1 : 0);
                }
            }
        }
    }

    /* Cursor */
    {
        int df = game.player_is_white ? game.cursor_file : (7 - game.cursor_file);
        int dr = game.player_is_white ? game.cursor_rank : (7 - game.cursor_rank);
        int cx = df * TILE_SIZE;
        int cy = dr * TILE_SIZE;
        draw_rect_outline(cx, cy, TILE_SIZE, TILE_SIZE, COLOR_CURSOR);
        draw_rect_outline(cx + 1, cy + 1, TILE_SIZE - 2, TILE_SIZE - 2, COLOR_CURSOR);
    }
}

/* ---- State machine ---- */

static void enter_state(game_state_t new_state) {
    game.state = new_state;
    game.think_frame = 0;
}

static int is_player_turn(void) {
    int side = engine_get_side();
    return (game.player_is_white && side == 0) ||
           (!game.player_is_white && side == 1);
}

static void update_legal_moves_for_selection(void) {
    game.legal_move_count = 0;

    if (game.engine == ENGINE_CHAL) {
        chal_move_info_t all_moves[256];
        int total = chal_get_legal_moves(all_moves, 256);
        uint8_t from = game_to_0x88(game.sel_rank, game.sel_file);
        for (int i = 0; i < total; i++) {
            if (all_moves[i].from == from)
                game.legal_moves[game.legal_move_count++] = all_moves[i];
        }
    } else {
        /* mcu-max: convert to chal_move_info_t with game-to-0x88 target mapping */
        mcumax_move all_moves[181];
        uint32_t total = mcumax_search_valid_moves(all_moves, 181);
        uint8_t from_sq = (game.sel_rank << 4) | game.sel_file;
        for (uint32_t i = 0; i < total; i++) {
            if (all_moves[i].from == from_sq) {
                chal_move_info_t m = { all_moves[i].from, all_moves[i].to, 0 };
                game.legal_moves[game.legal_move_count++] = m;
            }
        }
    }
}

static void check_game_end(void) {
    if (game.engine == ENGINE_CHAL) {
        if (chal_is_checkmate()) {
            game.result = 1;
            game.winner_is_white = (chal_get_side() == CHAL_BLACK);
            enter_state(STATE_GAME_OVER);
        } else if (chal_is_stalemate()) {
            game.result = 2;
            game.winner_is_white = -1;
            enter_state(STATE_GAME_OVER);
        }
    } else {
        mcumax_move moves[1];
        uint32_t count = mcumax_search_valid_moves(moves, 1);
        if (count == 0) {
            game.result = 1;
            game.winner_is_white = (engine_get_side() == 1);  /* black has no moves = white wins */
            enter_state(STATE_GAME_OVER);
        }
    }
}

static void do_player_select(void) {
    int piece = engine_get_piece(game.cursor_rank, game.cursor_file);

    if (game.selected) {
        /* Check if clicking on a valid target */
        if (is_valid_target(game.cursor_file, game.cursor_rank)) {
            /* Find the matching legal move */
            uint8_t to;
            if (game.engine == ENGINE_CHAL)
                to = game_to_0x88(game.cursor_rank, game.cursor_file);
            else
                to = (game.cursor_rank << 4) | game.cursor_file;

            int promo = 0;
            for (int i = 0; i < game.legal_move_count; i++) {
                if (game.legal_moves[i].to == to) {
                    promo = game.legal_moves[i].promo;
                    break;
                }
            }

            /* Check piece types BEFORE the move for sound selection */
            int moving_type = engine_get_piece(game.sel_rank, game.sel_file) & 0x7;
            int is_capture = (engine_get_piece(game.cursor_rank, game.cursor_file) & 0x7) != CHAL_EMPTY;

            bool ok;
            if (game.engine == ENGINE_CHAL) {
                uint8_t from = game_to_0x88(game.sel_rank, game.sel_file);
                ok = chal_play_move(from, to, promo);
            } else {
                uint8_t from = (game.sel_rank << 4) | game.sel_file;
                ok = mcumax_play_move((mcumax_move){from, to});
            }
            if (ok) {
                /* Play sound */
                if (is_capture) play_sound(game.snd_take);
                else if (moving_type == CHAL_PAWN) play_sound(game.snd_pawn);
                else play_sound(game.snd_move);

                game.has_last_move = 1;
                game.last_from_file = game.sel_file;
                game.last_from_rank = game.sel_rank;
                game.last_to_file = game.cursor_file;
                game.last_to_rank = game.cursor_rank;
                game.selected = 0;
                game.move_count++;
                update_eval();

                check_game_end();
                if (game.state != STATE_GAME_OVER) {
                    enter_state(STATE_AI_THINKING);
                }
                return;
            }
        }

        /* Clicked on own piece: re-select */
        int player_color_bit = game.player_is_white ? 0 : (CHAL_BLACK << 3);
        if ((piece & 0x7) != CHAL_EMPTY && (piece & (CHAL_BLACK << 3)) == player_color_bit) {
            game.sel_file = game.cursor_file;
            game.sel_rank = game.cursor_rank;
            update_legal_moves_for_selection();
            return;
        }

        /* Clicked on empty or enemy but not valid: deselect */
        game.selected = 0;
        game.legal_move_count = 0;
        return;
    }

    /* No piece selected: try to select one of ours */
    int player_color_bit = game.player_is_white ? 0 : (CHAL_BLACK << 3);
    if ((piece & 0x7) != CHAL_EMPTY && (piece & (CHAL_BLACK << 3)) == player_color_bit) {
        game.selected = 1;
        game.sel_file = game.cursor_file;
        game.sel_rank = game.cursor_rank;
        update_legal_moves_for_selection();
    }
}

/* Draw the starting position board as a backdrop (no cursor/highlights) */
static void draw_board_backdrop(void) {
    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            int screen_x = file * TILE_SIZE;
            int screen_y = rank * TILE_SIZE;

            int is_dark = (rank + file) % 2;
            blit_board_tile(screen_x, screen_y, is_dark);

            int piece = engine_get_piece(rank, file);
            if ((piece & 0x7) != CHAL_EMPTY) {
                int col_idx = piece_to_sprite_col(piece);
                int is_black = (piece & (CHAL_BLACK << 3)) != 0;
                if (col_idx >= 0) {
                    blit_sprite(screen_x, screen_y, col_idx, is_black ? 1 : 0);
                }
            }
        }
    }
}

/* Darken the entire screen by halving RGB channels */
static void darken_screen(void) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint16_t p = active_screen_buffer[i];
        /* Halve each channel: shift right 1 and mask to keep within field */
        uint16_t r = (p >> 12) & 0x0F;
        uint16_t g = ((p >> 6) & 0x1F);
        uint16_t b = (p >> 1) & 0x0F;
        active_screen_buffer[i] = (r << 11) | (g << 5) | b;
    }
}

/* ---- Eval bar (4px wide on left edge) ---- */
static void draw_eval_bar(void) {
    if (!game.show_eval_bar) return;

    /* Score from white's perspective. Chal: side-to-move, mcu-max: no eval. */
    int score = game.eval_score;
    /* Clamp to [-500, 500] centipawns for display */
    if (score > 500) score = 500;
    if (score < -500) score = -500;

    /* Map score to bar position: 0 = bottom (black winning), 128 = top (white winning) */
    int mid = SCREEN_H / 2;
    int bar_h = (score * mid) / 500;  /* positive = white, up */

    /* Draw background (dark) */
    fill_rect(0, 0, 4, SCREEN_H, 0x2104);

    /* Draw white portion (from bottom up to mid+bar_h) */
    int white_top = mid - bar_h;
    if (white_top < 0) white_top = 0;
    if (white_top > SCREEN_H) white_top = SCREEN_H;
    fill_rect(0, white_top, 4, SCREEN_H - white_top, 0xFFFF);

    /* Draw center line */
    for (int x = 0; x < 4; x++)
        active_screen_buffer[mid * SCREEN_W + x] = 0x8410;
}

static void update_eval(void) {
    if (game.engine == ENGINE_CHAL) {
        /* Get eval from side-to-move perspective, convert to white's perspective */
        int eval = chal_evaluate_position();
        game.eval_score = (chal_get_side() == CHAL_WHITE) ? eval : -eval;
    } else {
        game.eval_score = 0;  /* mcu-max doesn't expose eval */
    }
}

/* ---- Save/Load game via engine_save ---- */
static mp_obj_t save_fn = MP_OBJ_NULL;
static mp_obj_t load_fn = MP_OBJ_NULL;
static mp_obj_t set_location_fn = MP_OBJ_NULL;

static void init_save_module(void) {
    if (save_fn != MP_OBJ_NULL) return;
    mp_obj_t mod = mp_import_name(MP_QSTR_engine_save, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    save_fn = mp_load_attr(mod, MP_QSTR_save);
    load_fn = mp_load_attr(mod, MP_QSTR_load);
    set_location_fn = mp_load_attr(mod, MP_QSTR_set_location);
    /* Set save file location */
    mp_obj_t path = mp_obj_new_str("deepthumb.sav", 13);
    mp_call_function_1(set_location_fn, path);
}

static void save_game(void) {
    if (game.engine != ENGINE_CHAL) return;  /* only Chal supports FEN save */
    init_save_module();

    char fen[128];
    chal_get_fen(fen, sizeof(fen));

    mp_call_function_2(save_fn, mp_obj_new_str("fen", 3), mp_obj_new_str(fen, strlen(fen)));
    mp_call_function_2(save_fn, mp_obj_new_str("engine", 6), mp_obj_new_int(game.engine));
    mp_call_function_2(save_fn, mp_obj_new_str("diff", 4), mp_obj_new_int(game.difficulty));
    mp_call_function_2(save_fn, mp_obj_new_str("white", 5), mp_obj_new_int(game.player_is_white));
    mp_call_function_2(save_fn, mp_obj_new_str("moves", 5), mp_obj_new_int(game.move_count));
}

static int load_game(void) {
    init_save_module();

    /* Try to load FEN — if it doesn't exist, return 0 */
    mp_obj_t fen_obj = mp_call_function_2(load_fn, mp_obj_new_str("fen", 3), mp_const_none);
    if (fen_obj == mp_const_none) return 0;

    const char *fen = mp_obj_str_get_str(fen_obj);
    int eng = mp_obj_get_int(mp_call_function_2(load_fn, mp_obj_new_str("engine", 6), MP_OBJ_NEW_SMALL_INT(0)));
    int diff = mp_obj_get_int(mp_call_function_2(load_fn, mp_obj_new_str("diff", 4), MP_OBJ_NEW_SMALL_INT(1)));
    int white = mp_obj_get_int(mp_call_function_2(load_fn, mp_obj_new_str("white", 5), MP_OBJ_NEW_SMALL_INT(1)));
    int moves = mp_obj_get_int(mp_call_function_2(load_fn, mp_obj_new_str("moves", 5), MP_OBJ_NEW_SMALL_INT(0)));

    game.engine = eng;
    game.difficulty = diff;
    game.player_is_white = white;
    game.move_count = moves;

    chess_alloc_engine(game.engine);
    if (game.engine == ENGINE_CHAL) {
        chal_set_fen(fen);
    }

    game.cursor_file = 4;
    game.cursor_rank = game.player_is_white ? 6 : 1;
    game.selected = 0;
    game.legal_move_count = 0;
    game.has_last_move = 0;
    game.result = 0;
    update_eval();

    return 1;
}

static void clear_save(void) {
    init_save_module();
    mp_call_function_2(save_fn, mp_obj_new_str("fen", 3), mp_const_none);
}

/* ---- Pause menu ---- */
#define PAUSE_RESUME   0
#define PAUSE_SOUND    1
#define PAUSE_EVAL     2
#define PAUSE_SAVE     3
#define PAUSE_QUIT     4
#define PAUSE_COUNT    5

static void draw_pause_menu(void) {
    fill_rect(14, 16, 100, 96, COLOR_BG);
    draw_rect_outline(14, 16, 100, 96, COLOR_TEXT_WHITE);
    draw_text(36, 20, "PAUSED", COLOR_TEXT_WHITE);

    for (int x = 20; x < 108; x++)
        active_screen_buffer[29 * SCREEN_W + x] = COLOR_TEXT_DIM;

    const char *items[] = { "RESUME", "SOUND", "EVAL BAR", "SAVE+QUIT", "QUIT" };
    const char *vals[]  = { "", game.sound_on ? "ON" : "OFF",
                            game.show_eval_bar ? "ON" : "OFF", "", "" };

    for (int i = 0; i < PAUSE_COUNT; i++) {
        int y = 34 + i * 13;
        uint16_t color = (i == game.pause_cursor) ? COLOR_TEXT_WHITE : COLOR_TEXT_DIM;
        if (i == game.pause_cursor)
            fill_rect(18, y - 1, 92, 11, 0x2945);
        draw_text(24, y, items[i], color);
        if (vals[i][0]) draw_text(76, y, vals[i], 0x07E0);
    }
}

static int handle_pause_menu(void) {
    /* Returns: 0=stay, 1=quit to title, 2=save+quit */
    if (button_is_just_pressed(&BUTTON_DPAD_UP))
        game.pause_cursor = (game.pause_cursor + PAUSE_COUNT - 1) % PAUSE_COUNT;
    if (button_is_just_pressed(&BUTTON_DPAD_DOWN))
        game.pause_cursor = (game.pause_cursor + 1) % PAUSE_COUNT;
    if (button_is_just_pressed(&BUTTON_A)) {
        switch (game.pause_cursor) {
            case PAUSE_RESUME: game.paused = 0; break;
            case PAUSE_SOUND:  game.sound_on = !game.sound_on; break;
            case PAUSE_EVAL:   game.show_eval_bar = !game.show_eval_bar; break;
            case PAUSE_SAVE:   save_game(); return 1;
            case PAUSE_QUIT:   clear_save(); return 1;
        }
    }
    if (button_is_just_pressed(&BUTTON_MENU) || button_is_just_pressed(&BUTTON_B))
        game.paused = 0;
    return 0;
}

/* ---- Undo (Chal only: undo AI move + player move) ---- */
static void do_undo(void) {
    if (game.engine != ENGINE_CHAL) return;  /* mcu-max has no undo */
    if (game.move_count < 2) return;  /* need at least one full move pair */

    /* Undo AI's last move */
    if (chal_undo_move_api()) game.move_count--;
    /* Undo player's last move */
    if (chal_undo_move_api()) game.move_count--;

    game.selected = 0;
    game.legal_move_count = 0;
    game.has_last_move = 0;
    update_eval();
}

static void tick_title(void) {
    /* Ensure board is at starting position for the backdrop */
    if (game.think_frame == 0) {
        chess_alloc_engine(game.engine);
        engine_new_game();
        game.think_frame = 1;
    }

    /* Draw board as backdrop */
    draw_board_backdrop();
    darken_screen();

    /* Title text with shadow for readability */
    draw_text(29, 34, "DEEPTHUMB", 0x0000);
    draw_text(30, 33, "DEEPTHUMB", COLOR_TEXT_WHITE);

    draw_text(41, 47, "CHESS", 0x0000);
    draw_text(42, 46, "CHESS", COLOR_TEXT_DIM);

    /* Start / Continue prompts */
    fill_rect(16, 72, 96, 34, COLOR_BG);
    draw_rect_outline(16, 72, 96, 34, COLOR_TEXT_DIM);
    draw_text(30, 76, "A: NEW GAME", COLOR_TEXT_WHITE);
    draw_text(30, 88, "B: CONTINUE", COLOR_TEXT_DIM);

    if (button_is_just_pressed(&BUTTON_A)) {
        game.difficulty = 1;
        game.player_is_white = 1;
        enter_state(STATE_SETUP);
    }
    if (button_is_just_pressed(&BUTTON_B)) {
        /* Try to load saved game */
        if (load_game()) {
            enter_state(STATE_PLAYER_TURN);
        }
    }
}

static void tick_setup(void) {
    /* Board backdrop */
    draw_board_backdrop();
    darken_screen();

    /* Central panel */
    fill_rect(6, 8, 116, 112, COLOR_BG);
    draw_rect_outline(6, 8, 116, 112, COLOR_TEXT_DIM);

    draw_text(30, 12, "NEW GAME", COLOR_TEXT_WHITE);

    /* Separator */
    for (int x = 12; x < 116; x++)
        active_screen_buffer[21 * SCREEN_W + x] = COLOR_TEXT_DIM;

    /* Engine */
    draw_text(12, 25, "ENGINE", COLOR_TEXT_DIM);
    draw_text(55, 25, engine_names[game.engine], COLOR_TEXT_WHITE);

    /* Side — show piece icon */
    draw_text(12, 35, "SIDE", COLOR_TEXT_DIM);
    draw_text(55, 35, game.player_is_white ? "WHITE" : "BLACK", COLOR_TEXT_WHITE);
    blit_sprite(102, 31, SPRITE_KING, game.player_is_white ? 0 : 1);

    /* Difficulty */
    draw_text(12, 45, "LEVEL", COLOR_TEXT_DIM);
    draw_text(55, 45, diff_names[game.difficulty], COLOR_TEXT_WHITE);

    /* ELO estimate */
    const char **elo = (game.engine == ENGINE_CHAL) ? chal_elo : mcumax_elo;
    draw_text(12, 55, "ELO", COLOR_TEXT_DIM);
    draw_text(55, 55, elo[game.difficulty], 0x07E0);

    /* Separator */
    for (int x = 12; x < 116; x++)
        active_screen_buffer[65 * SCREEN_W + x] = COLOR_TEXT_DIM;

    /* Controls */
    draw_text(12, 69, "UP/DN  LEVEL", COLOR_TEXT_DIM);
    draw_text(12, 78, "LT/RT  SIDE", COLOR_TEXT_DIM);
    draw_text(12, 87, "B      ENGINE", COLOR_TEXT_DIM);

    /* Start button */
    fill_rect(28, 100, 72, 14, 0x2945);
    draw_rect_outline(28, 100, 72, 14, COLOR_TEXT_WHITE);
    draw_text(38, 104, "A: PLAY", COLOR_TEXT_WHITE);

    if (button_is_just_pressed(&BUTTON_DPAD_UP)) {
        game.difficulty = (game.difficulty + 1) % 4;
    }
    if (button_is_just_pressed(&BUTTON_DPAD_DOWN)) {
        game.difficulty = (game.difficulty + 3) % 4;
    }
    if (button_is_just_pressed(&BUTTON_BUMPER_LEFT) ||
        button_is_just_pressed(&BUTTON_BUMPER_RIGHT) ||
        button_is_just_pressed(&BUTTON_DPAD_LEFT) ||
        button_is_just_pressed(&BUTTON_DPAD_RIGHT)) {
        game.player_is_white = !game.player_is_white;
    }
    if (button_is_just_pressed(&BUTTON_B)) {
        game.engine = (game.engine + 1) % 2;
        /* Re-init the new engine so the backdrop board renders correctly */
        chess_alloc_engine(game.engine);
        engine_new_game();
    }

    if (button_is_just_pressed(&BUTTON_A)) {
        chess_alloc_engine(game.engine);
        engine_new_game();

        /* Sound init — done from Python via run_loop args */
        init_audio();

        game.cursor_file = 4;
        game.cursor_rank = game.player_is_white ? 6 : 1;
        game.selected = 0;
        game.legal_move_count = 0;
        game.has_last_move = 0;
        game.result = 0;

        if (is_player_turn()) {
            enter_state(STATE_PLAYER_TURN);
        } else {
            enter_state(STATE_AI_THINKING);
        }
    }
}

static void tick_player_turn(void) {
    /* Handle pause menu */
    if (game.paused) {
        draw_board();
        draw_eval_bar();
        darken_screen();
        draw_pause_menu();
        if (handle_pause_menu()) enter_state(STATE_TITLE);
        return;
    }

    draw_board();
    draw_eval_bar();

    /* MENU = pause */
    if (button_is_just_pressed(&BUTTON_MENU)) {
        game.paused = 1;
        game.pause_cursor = 0;
        return;
    }

    /* D-pad cursor movement — flip directions when board is flipped (playing black) */
    int rank_dec = game.player_is_white ? 7 : 1;
    int rank_inc = game.player_is_white ? 1 : 7;
    int file_dec = game.player_is_white ? 7 : 1;
    int file_inc = game.player_is_white ? 1 : 7;

    if (button_is_just_pressed(&BUTTON_DPAD_UP) || button_is_pressed_autorepeat(&BUTTON_DPAD_UP))
        game.cursor_rank = (game.cursor_rank + rank_dec) % 8;
    if (button_is_just_pressed(&BUTTON_DPAD_DOWN) || button_is_pressed_autorepeat(&BUTTON_DPAD_DOWN))
        game.cursor_rank = (game.cursor_rank + rank_inc) % 8;
    if (button_is_just_pressed(&BUTTON_DPAD_LEFT) || button_is_pressed_autorepeat(&BUTTON_DPAD_LEFT))
        game.cursor_file = (game.cursor_file + file_dec) % 8;
    if (button_is_just_pressed(&BUTTON_DPAD_RIGHT) || button_is_pressed_autorepeat(&BUTTON_DPAD_RIGHT))
        game.cursor_file = (game.cursor_file + file_inc) % 8;

    /* A = select/move */
    if (button_is_just_pressed(&BUTTON_A))
        do_player_select();

    /* B = undo if nothing selected, deselect if piece selected */
    if (button_is_just_pressed(&BUTTON_B)) {
        if (game.selected) {
            game.selected = 0;
            game.legal_move_count = 0;
        } else {
            do_undo();
        }
    }

    /* LB = undo (always) */
    if (button_is_just_pressed(&BUTTON_BUMPER_LEFT))
        do_undo();
}

static void tick_ai_thinking(void) {
    draw_board();
    fill_rect(0, SCREEN_H - 10, SCREEN_W, 10, COLOR_BG);
    draw_text(30, SCREEN_H - 9, "THINKING...", COLOR_TEXT_WHITE);

    /* Frame 0: just show the board with the player's move applied.
     * Frame 1: the display has been refreshed — now do the blocking search. */
    game.think_frame++;
    if (game.think_frame < 2) return;

    /* Perform blocking search (screen freezes until done) */
    if (game.engine == ENGINE_CHAL) {
        uint32_t depth = chal_depth[game.difficulty];
        uint32_t time_ms = chal_time[game.difficulty];
        chal_move_info_t best = chal_search_best_move(depth, time_ms);
        search_result_from = best.from;
        search_result_to = best.to;
        search_result_promo = best.promo;
        search_result_valid = (best.from != 0x80);
    } else {
        uint32_t nodes = mcumax_nodes[game.difficulty];
        uint32_t depth = mcumax_depth[game.difficulty];
        mcumax_move best = mcumax_search_best_move(nodes, depth);
        search_result_from = best.from;
        search_result_to = best.to;
        search_result_promo = 0;
        search_result_valid = (best.from != MCUMAX_SQUARE_INVALID);
    }

    /* Handle result */
    if (search_result_valid) {
        game.has_last_move = 1;
        if (game.engine == ENGINE_CHAL) {
            sq88_to_game(search_result_from, &game.last_from_rank, &game.last_from_file);
            sq88_to_game(search_result_to, &game.last_to_rank, &game.last_to_file);
            chal_play_move(search_result_from, search_result_to, search_result_promo);
        } else {
            game.last_from_file = search_result_from & 0x7;
            game.last_from_rank = search_result_from >> 4;
            game.last_to_file = search_result_to & 0x7;
            game.last_to_rank = search_result_to >> 4;
            mcumax_play_move((mcumax_move){search_result_from, search_result_to});
        }

        game.move_count++;
        play_sound(game.snd_move);
        update_eval();

        check_game_end();
        if (game.state != STATE_GAME_OVER) {
            enter_state(STATE_PLAYER_TURN);
        }
    } else {
        game.result = 2;
        game.winner_is_white = game.player_is_white;
        enter_state(STATE_GAME_OVER);
    }
}

static void tick_game_over(void) {
    draw_board();

    /* Overlay result */
    fill_rect(14, 42, 100, 44, COLOR_BG);
    draw_rect_outline(14, 42, 100, 44, COLOR_TEXT_WHITE);

    if (game.result >= 1) {
        if (game.winner_is_white == game.player_is_white) {
            draw_text(27, 48, "CHECKMATE!", COLOR_TEXT_WHITE);
            draw_text(33, 58, "YOU WIN!", 0x07E0);
        } else {
            draw_text(27, 48, "CHECKMATE!", COLOR_TEXT_WHITE);
            draw_text(30, 58, "YOU LOSE!", 0xF800);
        }
    }

    fill_rect(30, 70, 68, 12, 0x2945);
    draw_rect_outline(30, 70, 68, 12, COLOR_TEXT_DIM);
    draw_text(33, 72, "A: AGAIN", COLOR_TEXT_WHITE);

    if (button_is_just_pressed(&BUTTON_A)) {
        enter_state(STATE_TITLE);
    }
}

/* ---- Public API ---- */

void chess_game_init(uint16_t *sprite_data, int sprite_w, int sprite_h,
                     uint16_t *board_data, int board_w, int board_h,
                     mp_obj_t sound_move, mp_obj_t sound_take, mp_obj_t sound_pawn) {
    memset(&game, 0, sizeof(game));
    game.sprite_data = sprite_data;
    game.sprite_w = sprite_w;
    game.sprite_h = sprite_h;
    game.board_data = board_data;
    game.board_w = board_w;
    game.board_h = board_h;
    game.snd_move = sound_move;
    game.snd_take = sound_take;
    game.snd_pawn = sound_pawn;
    game.cursor_file = 4;
    game.cursor_rank = 6;
    game.difficulty = 1;
    game.player_is_white = 1;
    game.sound_on = 1;
    game.show_eval_bar = 0;
    enter_state(STATE_TITLE);
}

int chess_game_run_loop(void) {
    int frames = 0;

    while (1) {
        if (!engine_tick())
            continue;

        /* MENU exits to launcher from title/setup, handled in-game by pause menu */
        if (button_is_just_pressed(&BUTTON_MENU) &&
            (game.state == STATE_TITLE || game.state == STATE_SETUP))
            break;

        switch (game.state) {
            case STATE_TITLE:       tick_title(); break;
            case STATE_SETUP:       tick_setup(); break;
            case STATE_PLAYER_TURN: tick_player_turn(); break;
            case STATE_AI_THINKING: tick_ai_thinking(); break;
            case STATE_GAME_OVER:   tick_game_over(); break;
        }

        frames++;

        /* Handle MicroPython interrupts */
        mp_handle_pending(false);
        if (MP_STATE_THREAD(mp_pending_exception) != MP_OBJ_NULL)
            break;
    }

    return frames;
}
