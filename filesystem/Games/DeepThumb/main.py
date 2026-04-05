import engine_main
import engine
import chess_engine
from engine_resources import TextureResource, WaveSoundResource

engine.fps_limit(30)

sprite_tex = TextureResource("chess.bmp")
board_tex = TextureResource("board.bmp")

snd_move = WaveSoundResource("move.wav")
snd_take = WaveSoundResource("take.wav")
snd_pawn = WaveSoundResource("pawn.wav")

chess_engine.run_loop(sprite_tex, board_tex, snd_move, snd_take, snd_pawn)
