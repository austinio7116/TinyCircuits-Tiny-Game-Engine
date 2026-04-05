/*
 * chess_game.h — All-C game loop for DeepThumb chess
 */
#ifndef CHESS_GAME_H
#define CHESS_GAME_H

#include <stdint.h>

/* Initialize game state. Call before run_loop.
 * sprite_data: raw RGB565 pixel data from chess.bmp (96x32 = 6144 bytes)
 * board_data:  raw RGB565 pixel data from board.bmp (32x16 = 1024 bytes)
 * Both are 16-bit RGB565, bottom-up BMP layout. */
void chess_game_init(uint16_t *sprite_data, int sprite_w, int sprite_h,
                     uint16_t *board_data, int board_w, int board_h);

/* Run the all-C game loop. Returns when MENU is pressed. */
int chess_game_run_loop(void);

#endif
