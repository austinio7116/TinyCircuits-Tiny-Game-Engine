/*
 * chess_engine MicroPython C module
 * Wraps Chal chess engine for Thumby Color
 *
 * License: MIT
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/objarray.h"
#include "py/gc.h"
#include <string.h>
#include "chal.h"
#include "mcu-max.h"
#include "chess_game.h"

/* Buffers — allocated from MicroPython GC heap on demand */
#define CHAL_TT_COUNT 1024  /* 24KB — smaller to leave room for sounds + save */
#define MCUMAX_TT_COUNT 4096

/* Chal buffers */
static uint8_t *chal_tt_buf = NULL;
static size_t chal_tt_buf_size = 0;
static uint8_t *chal_dyn_buf = NULL;
static size_t chal_dyn_buf_size = 0;

/* mcu-max buffer */
static uint8_t *mcumax_tt_buf = NULL;
static size_t mcumax_tt_buf_size = 0;

/* Allocate only the selected engine to stay within memory budget */
static void chess_alloc_chal(void) {
    if (chal_dyn_buf != NULL) return;
    chal_dyn_buf_size = chal_get_dynamic_size();
    chal_dyn_buf = m_new(uint8_t, chal_dyn_buf_size);
    memset(chal_dyn_buf, 0, chal_dyn_buf_size);
    chal_set_dynamic_buffer(chal_dyn_buf);

    int entry_size = chal_get_tt_entry_size();
    chal_tt_buf_size = CHAL_TT_COUNT * entry_size;
    chal_tt_buf = m_new(uint8_t, chal_tt_buf_size);
    memset(chal_tt_buf, 0, chal_tt_buf_size);
    chal_set_tt(chal_tt_buf, CHAL_TT_COUNT);
    chal_init();
}

static void chess_alloc_mcumax(void) {
    if (mcumax_tt_buf != NULL) return;
    uint32_t entry_size = mcumax_get_hash_table_entry_size();
    mcumax_tt_buf_size = MCUMAX_TT_COUNT * entry_size;
    mcumax_tt_buf = m_new(uint8_t, mcumax_tt_buf_size);
    memset(mcumax_tt_buf, 0, mcumax_tt_buf_size);
    mcumax_set_hash_table(mcumax_tt_buf);
}

/* Called from chess_game.c when user selects an engine and starts a game */
void chess_alloc_engine(int engine) {
    if (engine == 0) {  /* ENGINE_CHAL */
        chess_alloc_chal();
    } else {            /* ENGINE_MCUMAX */
        chess_alloc_mcumax();
    }
}

static void chess_free(void) {
    chal_deinit();
    mcumax_deinit();
    if (chal_tt_buf != NULL) { m_del(uint8_t, chal_tt_buf, chal_tt_buf_size); chal_tt_buf = NULL; }
    if (chal_dyn_buf != NULL) { m_del(uint8_t, chal_dyn_buf, chal_dyn_buf_size); chal_dyn_buf = NULL; }
    if (mcumax_tt_buf != NULL) { m_del(uint8_t, mcumax_tt_buf, mcumax_tt_buf_size); mcumax_tt_buf = NULL; }
}

/* chess_engine.init() -> None */
static mp_obj_t chess_mp_init(void) {
    chess_alloc_chal();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_init_obj, chess_mp_init);

/* chess_engine.deinit() -> None */
static mp_obj_t chess_mp_deinit(void) {
    chess_free();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_deinit_obj, chess_mp_deinit);

/* chess_engine.new_game() -> None */
static mp_obj_t chess_mp_new_game(void) {
    chess_alloc_chal();
    chal_new_game();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_new_game_obj, chess_mp_new_game);

/* chess_engine.set_fen(fen_string) -> None */
static mp_obj_t chess_mp_set_fen(mp_obj_t fen_obj) {
    const char *fen = mp_obj_str_get_str(fen_obj);
    chal_set_fen(fen);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(chess_mp_set_fen_obj, chess_mp_set_fen);

/* chess_engine.get_piece(rank, file) -> int */
static mp_obj_t chess_mp_get_piece(mp_obj_t rank_obj, mp_obj_t file_obj) {
    int rank = mp_obj_get_int(rank_obj);
    int file = mp_obj_get_int(file_obj);
    return mp_obj_new_int(chal_get_piece(rank, file));
}
MP_DEFINE_CONST_FUN_OBJ_2(chess_mp_get_piece_obj, chess_mp_get_piece);

/* chess_engine.get_side() -> int (0=white, 1=black) */
static mp_obj_t chess_mp_get_side(void) {
    return mp_obj_new_int(chal_get_side());
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_get_side_obj, chess_mp_get_side);

/* chess_engine.get_legal_moves() -> list of (from, to, promo) tuples */
static mp_obj_t chess_mp_get_legal_moves(void) {
    chal_move_info_t moves[256];
    int count = chal_get_legal_moves(moves, 256);

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < count; i++) {
        mp_obj_t tuple[3] = {
            mp_obj_new_int(moves[i].from),
            mp_obj_new_int(moves[i].to),
            mp_obj_new_int(moves[i].promo),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(3, tuple));
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_get_legal_moves_obj, chess_mp_get_legal_moves);

/* chess_engine.make_move(from_sq, to_sq, promo) -> bool */
static mp_obj_t chess_mp_make_move(size_t n_args, const mp_obj_t *args) {
    int from = mp_obj_get_int(args[0]);
    int to = mp_obj_get_int(args[1]);
    int promo = (n_args >= 3) ? mp_obj_get_int(args[2]) : 0;
    return mp_obj_new_bool(chal_play_move(from, to, promo));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(chess_mp_make_move_obj, 2, 3, chess_mp_make_move);

/* chess_engine.get_ai_move(depth, time_ms) -> (from, to, promo) or None */
static mp_obj_t chess_mp_get_ai_move(size_t n_args, const mp_obj_t *args) {
    int depth = (n_args >= 1) ? mp_obj_get_int(args[0]) : 4;
    int time_ms = (n_args >= 2) ? mp_obj_get_int(args[1]) : 0;

    chal_move_info_t best = chal_search_best_move(depth, time_ms);
    if (best.from == 0x80) return mp_const_none;

    mp_obj_t tuple[3] = {
        mp_obj_new_int(best.from),
        mp_obj_new_int(best.to),
        mp_obj_new_int(best.promo),
    };
    return mp_obj_new_tuple(3, tuple);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(chess_mp_get_ai_move_obj, 0, 2, chess_mp_get_ai_move);

/* chess_engine.get_board() -> list of 64 ints */
static mp_obj_t chess_mp_get_board(void) {
    mp_obj_t items[64];
    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            items[rank * 8 + file] = mp_obj_new_int(chal_get_piece(rank, file));
        }
    }
    return mp_obj_new_list(64, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_get_board_obj, chess_mp_get_board);

/* chess_engine.is_game_over() -> int (0=no, 1=checkmate, 2=stalemate) */
static mp_obj_t chess_mp_is_game_over(void) {
    if (chal_is_checkmate()) return mp_obj_new_int(1);
    if (chal_is_stalemate()) return mp_obj_new_int(2);
    return mp_obj_new_int(0);
}
MP_DEFINE_CONST_FUN_OBJ_0(chess_mp_is_game_over_obj, chess_mp_is_game_over);

/* chess_engine.run_loop(sprite_tex, board_tex, snd_move, snd_take, snd_pawn) -> int */
static mp_obj_t chess_mp_run_loop(size_t n_args, const mp_obj_t *args) {
    mp_obj_t sprite_tex = args[0];
    mp_obj_t sprite_data_attr = mp_load_attr(sprite_tex, MP_QSTR_data);
    mp_buffer_info_t sprite_buf;
    mp_get_buffer_raise(sprite_data_attr, &sprite_buf, MP_BUFFER_READ);
    int sprite_w = mp_obj_get_int(mp_load_attr(sprite_tex, MP_QSTR_width));
    int sprite_h = mp_obj_get_int(mp_load_attr(sprite_tex, MP_QSTR_height));

    mp_obj_t board_tex = args[1];
    mp_obj_t board_data_attr = mp_load_attr(board_tex, MP_QSTR_data);
    mp_buffer_info_t board_buf;
    mp_get_buffer_raise(board_data_attr, &board_buf, MP_BUFFER_READ);
    int board_w = mp_obj_get_int(mp_load_attr(board_tex, MP_QSTR_width));
    int board_h = mp_obj_get_int(mp_load_attr(board_tex, MP_QSTR_height));

    /* Sound resources (optional) */
    mp_obj_t snd_move = (n_args >= 3) ? args[2] : mp_const_none;
    mp_obj_t snd_take = (n_args >= 4) ? args[3] : mp_const_none;
    mp_obj_t snd_pawn = (n_args >= 5) ? args[4] : mp_const_none;

    chess_game_init((uint16_t *)sprite_buf.buf, sprite_w, sprite_h,
                    (uint16_t *)board_buf.buf, board_w, board_h,
                    snd_move, snd_take, snd_pawn);
    int frames = chess_game_run_loop();
    chess_free();
    return mp_obj_new_int(frames);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(chess_mp_run_loop_obj, 2, 5, chess_mp_run_loop);

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
    { MP_OBJ_NEW_QSTR(MP_QSTR_run_loop),        MP_ROM_PTR(&chess_mp_run_loop_obj) },
    /* Piece type constants */
    { MP_OBJ_NEW_QSTR(MP_QSTR_EMPTY),           MP_OBJ_NEW_SMALL_INT(CHAL_EMPTY) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PAWN),            MP_OBJ_NEW_SMALL_INT(CHAL_PAWN) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_KNIGHT),          MP_OBJ_NEW_SMALL_INT(CHAL_KNIGHT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BISHOP),          MP_OBJ_NEW_SMALL_INT(CHAL_BISHOP) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_ROOK),            MP_OBJ_NEW_SMALL_INT(CHAL_ROOK) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_QUEEN),           MP_OBJ_NEW_SMALL_INT(CHAL_QUEEN) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_KING),            MP_OBJ_NEW_SMALL_INT(CHAL_KING) },
    /* Side constants */
    { MP_OBJ_NEW_QSTR(MP_QSTR_WHITE),           MP_OBJ_NEW_SMALL_INT(CHAL_WHITE) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_BLACK),           MP_OBJ_NEW_SMALL_INT(CHAL_BLACK) },
};

static MP_DEFINE_CONST_DICT(mp_module_chess_globals, chess_globals_table);

const mp_obj_module_t chess_engine_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_chess_globals,
};

MP_REGISTER_MODULE(MP_QSTR_chess_engine, chess_engine_user_cmodule);
