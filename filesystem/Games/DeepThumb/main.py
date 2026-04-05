import engine_main
import engine
import chess_engine
from engine_resources import TextureResource

engine.fps_limit(30)

# Load sprite textures — passed to C game loop for direct framebuffer rendering
sprite_tex = TextureResource("chess.bmp")
board_tex = TextureResource("board.bmp")

# Enter the all-C game loop (returns when MENU is pressed)
chess_engine.run_loop(sprite_tex, board_tex)
