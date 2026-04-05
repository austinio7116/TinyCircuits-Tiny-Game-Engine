/*
 * chess_engine MicroPython C module
 * Wraps mcu-max chess engine for Thumby Color
 *
 * License: MIT
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/objarray.h"
#include "py/gc.h"
#include <string.h>
#include "mcu-max.h"
#include "chess_game.h"

/* Hash table buffer — allocated from MicroPython GC heap */
#ifndef MCUMAX_HASH_TABLE_SIZE
#define MCUMAX_HASH_TABLE_SIZE 4096
#endif
static uint8_t *hash_table_buf = NULL;
static size_t hash_table_buf_size = 0;

static void chess_alloc_hash_table(void) {
    if (hash_table_buf != NULL) return;  /* already allocated */
    uint32_t entry_size = mcumax_get_hash_table_entry_size();
    hash_table_buf_size = MCUMAX_HASH_TABLE_SIZE * entry_size;
    hash_table_buf = m_new(uint8_t, hash_table_buf_size);
    memset(hash_table_buf, 0, hash_table_buf_size);
    mcumax_set_hash_table(hash_table_buf);
}

static void chess_free_hash_table(void) {
    if (hash_table_buf == NULL) return;
    mcumax_deinit();  /* clear the pointer in mcu-max */
    m_del(uint8_t, hash_table_buf, hash_table_buf_size);
    hash_table_buf = NULL;
    hash_table_buf_size = 0;
}

/* chess_engine.init() -> None
 * Initialize engine with starting position. */
static mp_obj_t chess_mp_init(void) {
    chess_alloc_hash_table();
    mcumax_init();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_init_obj, chess_mp_init);

/* chess_engine.deinit() -> None
 * Free dynamically allocated memory (hash table). */
static mp_obj_t chess_mp_deinit(void) {
    chess_free_hash_table();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_deinit_obj, chess_mp_deinit);

/* chess_engine.new_game() -> None
 * Reset to starting position. */
static mp_obj_t chess_mp_new_game(void) {
    mcumax_init();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_new_game_obj, chess_mp_new_game);

/* chess_engine.set_fen(fen_string) -> None
 * Load position from FEN string. */
static mp_obj_t chess_mp_set_fen(mp_obj_t fen_obj) {
    const char *fen = mp_obj_str_get_str(fen_obj);
    mcumax_set_fen_position(fen);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(chess_mp_set_fen_obj, chess_mp_set_fen);

/* chess_engine.get_piece(square) -> int
 * Get piece at square (0xRF format: rank<<4 | file).
 * Returns piece code (0=empty, bits 0-2=type, bit 3=black). */
static mp_obj_t chess_mp_get_piece(mp_obj_t sq_obj) {
    uint8_t sq = (uint8_t)mp_obj_get_int(sq_obj);
    return mp_obj_new_int(mcumax_get_piece(sq));
}
MP_DEFINE_CONST_FUN_OBJ_1(chess_mp_get_piece_obj, chess_mp_get_piece);

/* chess_engine.get_side() -> int
 * Returns current side to move (WHITE=0x8, BLACK=0x10). */
static mp_obj_t chess_mp_get_side(void) {
    return mp_obj_new_int(mcumax_get_current_side());
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_get_side_obj, chess_mp_get_side);

/* chess_engine.get_legal_moves() -> list of (from, to) tuples
 * Returns all legal moves for current side. */
static mp_obj_t chess_mp_get_legal_moves(void) {
    mcumax_move moves[181];
    uint32_t count = mcumax_search_valid_moves(moves, 181);

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (uint32_t i = 0; i < count; i++) {
        mp_obj_t tuple[2] = {
            mp_obj_new_int(moves[i].from),
            mp_obj_new_int(moves[i].to),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(2, tuple));
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_get_legal_moves_obj, chess_mp_get_legal_moves);

/* chess_engine.make_move(from_sq, to_sq) -> bool
 * Play a move. Returns True if legal. */
static mp_obj_t chess_mp_make_move(mp_obj_t from_obj, mp_obj_t to_obj) {
    mcumax_move move = {
        .from = (uint8_t)mp_obj_get_int(from_obj),
        .to = (uint8_t)mp_obj_get_int(to_obj),
    };
    bool ok = mcumax_play_move(move);
    return mp_obj_new_bool(ok);
}
MP_DEFINE_CONST_FUN_OBJ_2(chess_mp_make_move_obj, chess_mp_make_move);

/* chess_engine.get_ai_move(node_limit, depth_limit) -> (from, to) or None
 * Search for best move. Blocking call.
 * node_limit: max nodes to search (0 = unlimited)
 * depth_limit: max search depth */
static mp_obj_t chess_mp_get_ai_move(size_t n_args, const mp_obj_t *args) {
    uint32_t node_limit = 100000;
    uint32_t depth_limit = 6;

    if (n_args >= 1) {
        node_limit = (uint32_t)mp_obj_get_int(args[0]);
    }
    if (n_args >= 2) {
        depth_limit = (uint32_t)mp_obj_get_int(args[1]);
    }

    mcumax_move best = mcumax_search_best_move(node_limit, depth_limit);

    if (best.from == MCUMAX_SQUARE_INVALID) {
        return mp_const_none;
    }

    mp_obj_t tuple[2] = {
        mp_obj_new_int(best.from),
        mp_obj_new_int(best.to),
    };
    return mp_obj_new_tuple(2, tuple);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(chess_mp_get_ai_move_obj, 0, 2, chess_mp_get_ai_move);

/* chess_engine.get_board() -> list of 64 ints
 * Returns board state as 64 piece codes, a1-h1 then a2-h2 etc.
 * Each value: 0=empty, or piece type | color bit. */
static mp_obj_t chess_mp_get_board(void) {
    mp_obj_t items[64];
    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            mcumax_square sq = (rank << 4) | file;
            items[rank * 8 + file] = mp_obj_new_int(mcumax_get_piece(sq));
        }
    }
    return mp_obj_new_list(64, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_get_board_obj, chess_mp_get_board);

/* chess_engine.is_game_over() -> int
 * Returns 0 if game continues, 1 if current side has no legal moves.
 * (Checkmate or stalemate — caller must check context.) */
static mp_obj_t chess_mp_is_game_over(void) {
    mcumax_move moves[1];
    uint32_t count = mcumax_search_valid_moves(moves, 1);
    return mp_obj_new_int(count == 0 ? 1 : 0);
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_is_game_over_obj, chess_mp_is_game_over);

/* chess_engine.run_loop(sprite_texture, board_texture) -> int
 * Run the all-C game loop. Textures are TextureResource objects.
 * Returns frame count when MENU is pressed. */
static mp_obj_t chess_mp_run_loop(size_t n_args, const mp_obj_t *args) {
    /* Get sprite texture data buffer */
    mp_obj_t sprite_tex = args[0];
    mp_obj_t sprite_data_attr = mp_load_attr(sprite_tex, MP_QSTR_data);
    mp_buffer_info_t sprite_buf;
    mp_get_buffer_raise(sprite_data_attr, &sprite_buf, MP_BUFFER_READ);
    int sprite_w = mp_obj_get_int(mp_load_attr(sprite_tex, MP_QSTR_width));
    int sprite_h = mp_obj_get_int(mp_load_attr(sprite_tex, MP_QSTR_height));

    /* Get board texture data buffer */
    mp_obj_t board_tex = args[1];
    mp_obj_t board_data_attr = mp_load_attr(board_tex, MP_QSTR_data);
    mp_buffer_info_t board_buf;
    mp_get_buffer_raise(board_data_attr, &board_buf, MP_BUFFER_READ);
    int board_w = mp_obj_get_int(mp_load_attr(board_tex, MP_QSTR_width));
    int board_h = mp_obj_get_int(mp_load_attr(board_tex, MP_QSTR_height));

    /* Allocate hash table before any engine calls */
    chess_alloc_hash_table();

    chess_game_init((uint16_t *)sprite_buf.buf, sprite_w, sprite_h,
                    (uint16_t *)board_buf.buf, board_w, board_h);
    int frames = chess_game_run_loop();
    /* Free hash table memory when leaving chess */
    mcumax_deinit();
    return mp_obj_new_int(frames);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(chess_mp_run_loop_obj, 2, 2, chess_mp_run_loop);

/* Module globals table */
static const mp_rom_map_elem_t chess_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__),        MP_OBJ_NEW_QSTR(MP_QSTR_chess_engine) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_init),            MP_ROM_PTR(&chess_mp_init_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit),          MP_ROM_PTR(&chess_mp_deinit_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_new_game),        MP_ROM_PTR(&chess_mp_new_game_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_set_fen),         MP_ROM_PTR(&chess_mp_set_fen_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_piece),       MP_ROM_PTR(&chess_mp_get_piece_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_side),        MP_ROM_PTR(&chess_mp_get_side_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_legal_moves), MP_ROM_PTR(&chess_mp_get_legal_moves_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_make_move),       MP_ROM_PTR(&chess_mp_make_move_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_ai_move),     MP_ROM_PTR(&chess_mp_get_ai_move_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_board),       MP_ROM_PTR(&chess_mp_get_board_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_is_game_over),    MP_ROM_PTR(&chess_mp_is_game_over_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_run_loop),       MP_ROM_PTR(&chess_mp_run_loop_obj) },
    /* Piece type constants */
    { MP_OBJ_NEW_QSTR(MP_QSTR_EMPTY),           MP_OBJ_NEW_SMALL_INT(MCUMAX_EMPTY) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PAWN),            MP_OBJ_NEW_SMALL_INT(MCUMAX_PAWN_UPSTREAM) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_KNIGHT),          MP_OBJ_NEW_SMALL_INT(MCUMAX_KNIGHT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BISHOP),          MP_OBJ_NEW_SMALL_INT(MCUMAX_BISHOP) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_ROOK),            MP_OBJ_NEW_SMALL_INT(MCUMAX_ROOK) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_QUEEN),           MP_OBJ_NEW_SMALL_INT(MCUMAX_QUEEN) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_KING),            MP_OBJ_NEW_SMALL_INT(MCUMAX_KING) },
    /* Side constants */
    { MP_OBJ_NEW_QSTR(MP_QSTR_WHITE),           MP_OBJ_NEW_SMALL_INT(0x8) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BLACK),           MP_OBJ_NEW_SMALL_INT(0x10) },
};

static MP_DEFINE_CONST_DICT(mp_module_chess_globals, chess_globals_table);

const mp_obj_module_t chess_engine_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_chess_globals,
};

MP_REGISTER_MODULE(MP_QSTR_chess_engine, chess_engine_user_cmodule);
