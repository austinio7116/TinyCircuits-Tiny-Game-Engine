/*
 * chess_game.c — All-C game loop for DeepThumb chess
 *
 * Renders directly to the engine's framebuffer, polls buttons,
 * and drives the mcu-max chess engine for AI moves.
 */

#include "chess_game.h"
#include "mcu-max.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "io/engine_io_buttons.h"
#include <string.h>

/* Dual-core support: RP2350 only */
#if defined(__arm__)
#define CHESS_DUAL_CORE 1
#include "pico/multicore.h"
#include "pico/time.h"
#else
#define CHESS_DUAL_CORE 0
#endif

/* ---- Dual-core AI search ---- */

#define SEARCH_IDLE      0
#define SEARCH_RUNNING   1
#define SEARCH_DONE      2

static volatile int search_state = SEARCH_IDLE;
static volatile uint8_t search_result_from;
static volatile uint8_t search_result_to;
static volatile int search_result_valid;  /* 1 = move found, 0 = no moves */

#if CHESS_DUAL_CORE
/* Search parameters — set by core 0 before launching core 1 */
static uint32_t search_node_limit;
static uint32_t search_depth_limit;
/* Core 1 stack — 8KB for recursive search */
static uint32_t core1_stack[2048];  /* 8KB */
static int core1_launched = 0;

static void core1_search_entry(void) {
    while (1) {
        /* Wait for search command from core 0 */
        uint32_t cmd = multicore_fifo_pop_blocking();
        (void)cmd;

        search_state = SEARCH_RUNNING;
        mcumax_move best = mcumax_search_best_move(search_node_limit, search_depth_limit);

        if (best.from != MCUMAX_SQUARE_INVALID) {
            search_result_from = best.from;
            search_result_to = best.to;
            search_result_valid = 1;
        } else {
            search_result_valid = 0;
        }
        search_state = SEARCH_DONE;
    }
}

static void launch_core1_if_needed(void) {
    if (!core1_launched) {
        multicore_launch_core1_with_stack(core1_search_entry,
            core1_stack, sizeof(core1_stack));
        core1_launched = 1;
    }
}

static void start_search_async(uint32_t nodes, uint32_t depth) {
    launch_core1_if_needed();
    search_node_limit = nodes;
    search_depth_limit = depth;
    search_state = SEARCH_RUNNING;
    multicore_fifo_push_blocking(1);  /* signal core 1 to start */
}
#endif /* CHESS_DUAL_CORE */

/* Start search — async on device, blocking on emulator */
static void start_ai_search(uint32_t nodes, uint32_t depth) {
#if CHESS_DUAL_CORE
    start_search_async(nodes, depth);
#else
    /* Blocking fallback for unix emulator */
    search_state = SEARCH_RUNNING;
    mcumax_move best = mcumax_search_best_move(nodes, depth);
    search_result_from = best.from;
    search_result_to = best.to;
    search_result_valid = (best.from != MCUMAX_SQUARE_INVALID);
    search_state = SEARCH_DONE;
#endif
}

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
    int difficulty;         /* 0=easy(d3), 1=medium(d4), 2=hard(d5), 3=expert(d6) */

    /* Cursor */
    int cursor_file;        /* 0-7 */
    int cursor_rank;        /* 0-7 */

    /* Selection */
    int selected;           /* 1 if a piece is selected */
    int sel_file, sel_rank;

    /* Legal moves for selected piece */
    mcumax_move legal_moves[181];
    int legal_move_count;

    /* Last move highlight */
    int has_last_move;
    int last_from_file, last_from_rank;
    int last_to_file, last_to_rank;

    /* Game result */
    int result;  /* 0=ongoing, 1=checkmate, 2=stalemate */
    int winner_is_white;

    /* AI thinking animation */
    int think_dots;
    int think_frame;

    /* Sprite data */
    uint16_t *sprite_data;
    int sprite_w, sprite_h;
    uint16_t *board_data;
    int board_w, board_h;

    /* Board snapshot — used during AI thinking to avoid rendering search internals */
    uint8_t board_snapshot[64];  /* piece codes, rank-major */
    int use_snapshot;
} game;

/* Difficulty settings: node limit and depth limit */
static const uint32_t diff_nodes[] = { 10000, 50000, 200000, 500000 };
static const uint32_t diff_depth[] = { 3, 4, 5, 6 };
static const char *diff_names[] = { "Easy", "Medium", "Hard", "Expert" };

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

/* Take a snapshot of the current board for rendering during AI search */
static void take_board_snapshot(void) {
    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            mcumax_square sq = (rank << 4) | file;
            game.board_snapshot[rank * 8 + file] = mcumax_get_piece(sq);
        }
    }
    game.use_snapshot = 1;
}

/* Get piece at (file, rank) — uses snapshot if active, otherwise live board */
static uint8_t get_piece_for_render(int file, int rank) {
    if (game.use_snapshot) {
        return game.board_snapshot[rank * 8 + file];
    }
    mcumax_square sq = (rank << 4) | file;
    return mcumax_get_piece(sq);
}

/* Convert mcu-max piece to sprite column index.
 * mcu-max pieces (after get_piece XOR): type in bits 0-2, black in bit 3.
 * Types: 0=empty, 1=pawn_up, 2=pawn_down, 3=knight, 4=king, 5=bishop, 6=rook, 7=queen */
static int piece_to_sprite_col(uint8_t piece) {
    int ptype = piece & 0x7;
    switch (ptype) {
        case 1: case 2: return SPRITE_PAWN;
        case 3: return SPRITE_KNIGHT;
        case 4: return SPRITE_KING;
        case 5: return SPRITE_BISHOP;
        case 6: return SPRITE_ROOK;
        case 7: return SPRITE_QUEEN;
        default: return -1;
    }
}

/* Is a move in the legal moves list for the selected piece? */
static int is_valid_target(int file, int rank) {
    mcumax_square to = (rank << 4) | file;
    for (int i = 0; i < game.legal_move_count; i++) {
        if (game.legal_moves[i].to == to)
            return 1;
    }
    return 0;
}

/* Check if the current side's king is in check.
 * Heuristic: get legal moves. If a capture of the king is possible,
 * we're in check. mcu-max doesn't expose is_check directly.
 * Alternative: check if any opponent piece attacks the king square.
 * Simplest: we just skip check highlighting for now. */

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
                mcumax_piece p = get_piece_for_render(file, rank);
                if ((p & 0x7) == MCUMAX_EMPTY) {
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
            mcumax_piece piece = get_piece_for_render(file, rank);
            if ((piece & 0x7) != MCUMAX_EMPTY) {
                int col_idx = piece_to_sprite_col(piece);
                int is_black = (piece & MCUMAX_BLACK) != 0;
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
    game.think_dots = 0;
}

static int is_player_turn(void) {
    uint8_t side = mcumax_get_current_side();
    return (game.player_is_white && side == 0x8) ||
           (!game.player_is_white && side == 0x10);
}

static void update_legal_moves_for_selection(void) {
    /* Get all legal moves, then filter to those from the selected square */
    mcumax_move all_moves[181];
    uint32_t total = mcumax_search_valid_moves(all_moves, 181);

    mcumax_square from = (game.sel_rank << 4) | game.sel_file;
    game.legal_move_count = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (all_moves[i].from == from) {
            game.legal_moves[game.legal_move_count++] = all_moves[i];
        }
    }
}

static void check_game_end(void) {
    mcumax_move moves[1];
    uint32_t count = mcumax_search_valid_moves(moves, 1);
    if (count == 0) {
        /* No legal moves: checkmate or stalemate.
         * mcu-max doesn't distinguish, but if the side has no moves
         * and is in check it's checkmate, otherwise stalemate.
         * For simplicity, treat all no-move situations as game over. */
        game.result = 1;  /* game ended */
        /* The side that ran out of moves lost (or it's stalemate) */
        uint8_t side = mcumax_get_current_side();
        game.winner_is_white = (side == 0x10);  /* black has no moves = white wins */
        enter_state(STATE_GAME_OVER);
    }
}

static void do_player_select(void) {
    mcumax_square sq = (game.cursor_rank << 4) | game.cursor_file;
    mcumax_piece piece = mcumax_get_piece(sq);

    if (game.selected) {
        /* Check if clicking on a valid target */
        if (is_valid_target(game.cursor_file, game.cursor_rank)) {
            /* Make the move */
            mcumax_square from = (game.sel_rank << 4) | game.sel_file;
            mcumax_square to = sq;
            bool ok = mcumax_play_move((mcumax_move){from, to});
            if (ok) {
                game.has_last_move = 1;
                game.last_from_file = game.sel_file;
                game.last_from_rank = game.sel_rank;
                game.last_to_file = game.cursor_file;
                game.last_to_rank = game.cursor_rank;
                game.selected = 0;

                /* Check for game end */
                check_game_end();
                if (game.state != STATE_GAME_OVER) {
                    enter_state(STATE_AI_THINKING);
                }
                return;
            }
        }

        /* Clicked on own piece: re-select */
        int player_color_bit = game.player_is_white ? 0 : MCUMAX_BLACK;
        if ((piece & 0x7) != MCUMAX_EMPTY && (piece & MCUMAX_BLACK) == player_color_bit) {
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
    int player_color_bit = game.player_is_white ? 0 : MCUMAX_BLACK;
    if ((piece & 0x7) != MCUMAX_EMPTY && (piece & MCUMAX_BLACK) == player_color_bit) {
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

            mcumax_square sq = (rank << 4) | file;
            mcumax_piece piece = mcumax_get_piece(sq);
            if ((piece & 0x7) != MCUMAX_EMPTY) {
                int col_idx = piece_to_sprite_col(piece);
                int is_black = (piece & MCUMAX_BLACK) != 0;
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

static void tick_title(void) {
    /* Ensure board is at starting position for the backdrop */
    if (game.think_frame == 0) {
        mcumax_init();
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

    /* Start prompt */
    fill_rect(20, 78, 88, 20, COLOR_BG);
    draw_rect_outline(20, 78, 88, 20, COLOR_TEXT_DIM);
    draw_text(30, 82, "PRESS A", COLOR_TEXT_WHITE);
    draw_text(30, 90, "TO PLAY", COLOR_TEXT_WHITE);

    if (button_is_just_pressed(&BUTTON_A)) {
        game.difficulty = 1;  /* default medium */
        game.player_is_white = 1;
        enter_state(STATE_SETUP);
    }
}

static void tick_setup(void) {
    /* Board backdrop */
    draw_board_backdrop();
    darken_screen();

    /* Central panel */
    fill_rect(8, 12, 112, 104, COLOR_BG);
    draw_rect_outline(8, 12, 112, 104, COLOR_TEXT_DIM);

    draw_text(30, 17, "NEW GAME", COLOR_TEXT_WHITE);

    /* Horizontal line */
    for (int x = 14; x < 114; x++)
        active_screen_buffer[27 * SCREEN_W + x] = COLOR_TEXT_DIM;

    /* Color selection — show piece icon */
    draw_text(15, 34, "SIDE", COLOR_TEXT_DIM);
    draw_text(50, 34, game.player_is_white ? "WHITE" : "BLACK", COLOR_TEXT_WHITE);
    /* Draw a small king icon for the selected color */
    blit_sprite(100, 30, SPRITE_KING, game.player_is_white ? 0 : 1);

    /* Difficulty */
    draw_text(15, 52, "LEVEL", COLOR_TEXT_DIM);
    draw_text(50, 52, diff_names[game.difficulty], COLOR_TEXT_WHITE);

    /* Separator */
    for (int x = 14; x < 114; x++)
        active_screen_buffer[66 * SCREEN_W + x] = COLOR_TEXT_DIM;

    /* Controls */
    draw_text(15, 72, "UP/DN  LEVEL", COLOR_TEXT_DIM);
    draw_text(15, 82, "LB/RB  SIDE", COLOR_TEXT_DIM);

    /* Start button */
    fill_rect(30, 96, 68, 14, 0x2945);
    draw_rect_outline(30, 96, 68, 14, COLOR_TEXT_WHITE);
    draw_text(40, 100, "A: PLAY", COLOR_TEXT_WHITE);

    if (button_is_just_pressed(&BUTTON_DPAD_UP)) {
        game.difficulty = (game.difficulty + 1) % 4;
    }
    if (button_is_just_pressed(&BUTTON_DPAD_DOWN)) {
        game.difficulty = (game.difficulty + 3) % 4;
    }
    if (button_is_just_pressed(&BUTTON_BUMPER_LEFT) ||
        button_is_just_pressed(&BUTTON_BUMPER_RIGHT)) {
        game.player_is_white = !game.player_is_white;
    }

    if (button_is_just_pressed(&BUTTON_A)) {
        mcumax_init();
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
    draw_board();

    /* D-pad cursor movement — flip directions when board is flipped (playing black) */
    int rank_dec = game.player_is_white ? 7 : 1;  /* visual "up" */
    int rank_inc = game.player_is_white ? 1 : 7;  /* visual "down" */
    int file_dec = game.player_is_white ? 7 : 1;  /* visual "left" */
    int file_inc = game.player_is_white ? 1 : 7;  /* visual "right" */

    if (button_is_just_pressed(&BUTTON_DPAD_UP) || button_is_pressed_autorepeat(&BUTTON_DPAD_UP)) {
        game.cursor_rank = (game.cursor_rank + rank_dec) % 8;
    }
    if (button_is_just_pressed(&BUTTON_DPAD_DOWN) || button_is_pressed_autorepeat(&BUTTON_DPAD_DOWN)) {
        game.cursor_rank = (game.cursor_rank + rank_inc) % 8;
    }
    if (button_is_just_pressed(&BUTTON_DPAD_LEFT) || button_is_pressed_autorepeat(&BUTTON_DPAD_LEFT)) {
        game.cursor_file = (game.cursor_file + file_dec) % 8;
    }
    if (button_is_just_pressed(&BUTTON_DPAD_RIGHT) || button_is_pressed_autorepeat(&BUTTON_DPAD_RIGHT)) {
        game.cursor_file = (game.cursor_file + file_inc) % 8;
    }

    /* A = select/move */
    if (button_is_just_pressed(&BUTTON_A)) {
        do_player_select();
    }

    /* B = deselect */
    if (button_is_just_pressed(&BUTTON_B)) {
        game.selected = 0;
        game.legal_move_count = 0;
    }
}

static void tick_ai_thinking(void) {
    draw_board();

    /* Animated "thinking" indicator */
    game.think_frame++;
    if (game.think_frame % 10 == 0) {
        game.think_dots = (game.think_dots + 1) % 4;
    }

    /* Draw thinking text overlay at bottom */
    fill_rect(0, SCREEN_H - 10, SCREEN_W, 10, COLOR_BG);
    char buf[20] = "THINKING";
    for (int i = 0; i < game.think_dots; i++) {
        buf[8 + i] = '.';
    }
    buf[8 + game.think_dots] = '\0';
    draw_text(35, SCREEN_H - 9, buf, COLOR_TEXT_WHITE);

    /* Frame 1: snapshot the board and launch the search */
    if (game.think_frame == 1) {
        take_board_snapshot();  /* freeze board for rendering while search runs */
        uint32_t nodes = diff_nodes[game.difficulty];
        uint32_t depth = diff_depth[game.difficulty];
        start_ai_search(nodes, depth);
    }

    /* Poll for search completion */
    if (search_state == SEARCH_DONE) {
        game.use_snapshot = 0;  /* back to live board */
        if (search_result_valid) {
            /* Record move for highlight */
            game.has_last_move = 1;
            game.last_from_file = search_result_from & 0x7;
            game.last_from_rank = search_result_from >> 4;
            game.last_to_file = search_result_to & 0x7;
            game.last_to_rank = search_result_to >> 4;

            mcumax_play_move((mcumax_move){search_result_from, search_result_to});

            check_game_end();
            if (game.state != STATE_GAME_OVER) {
                enter_state(STATE_PLAYER_TURN);
            }
        } else {
            /* AI has no moves */
            game.result = 2;  /* stalemate or mate */
            game.winner_is_white = game.player_is_white;
            enter_state(STATE_GAME_OVER);
        }
        search_state = SEARCH_IDLE;
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
                     uint16_t *board_data, int board_w, int board_h) {
    memset(&game, 0, sizeof(game));
    game.sprite_data = sprite_data;
    game.sprite_w = sprite_w;
    game.sprite_h = sprite_h;
    game.board_data = board_data;
    game.board_w = board_w;
    game.board_h = board_h;
    game.cursor_file = 4;
    game.cursor_rank = 6;
    game.difficulty = 1;
    game.player_is_white = 1;
    enter_state(STATE_TITLE);
}

int chess_game_run_loop(void) {
    int frames = 0;

    while (1) {
        if (!engine_tick())
            continue;

        if (button_is_just_pressed(&BUTTON_MENU)) {
#if CHESS_DUAL_CORE
            /* Abort any running search before exiting */
            if (search_state == SEARCH_RUNNING)
                mcumax_stop_search();
#endif
            break;
        }

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

#if CHESS_DUAL_CORE
    /* Stop core 1 when exiting the game loop */
    if (core1_launched) {
        /* If a search is still running, abort it */
        if (search_state == SEARCH_RUNNING) {
            mcumax_stop_search();
            /* Wait for it to finish */
            while (search_state == SEARCH_RUNNING) {
                tight_loop_contents();
            }
        }
        multicore_reset_core1();
        core1_launched = 0;
    }
#endif
    search_state = SEARCH_IDLE;

    return frames;
}
