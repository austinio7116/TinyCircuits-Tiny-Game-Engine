/*
 * chess_game.h — All-C game loop for DeepThumb chess
 */
#ifndef CHESS_GAME_H
#define CHESS_GAME_H

#include <stdint.h>
#include "py/obj.h"

/* Initialize game state. Call before run_loop.
 * sprite_data/board_data: raw RGB565 pixel data from BMP textures.
 * sound_move/take/pawn: MicroPython WaveSoundResource objects (or mp_const_none). */
void chess_game_init(uint16_t *sprite_data, int sprite_w, int sprite_h,
                     uint16_t *board_data, int board_w, int board_h,
                     mp_obj_t sound_move, mp_obj_t sound_take, mp_obj_t sound_pawn);

/* Run the all-C game loop. Returns when MENU exits to launcher. */
int chess_game_run_loop(void);

/* Called from chess_game.c when user selects an engine.
 * Allocates memory for the selected engine only.
 * engine: 0 = CHAL, 1 = MCU-MAX */
extern void chess_alloc_engine(int engine);

#endif
