import engine_main
import engine
import gc
import sys

# Delete all unnecessary modules to free GC heap
# The launcher imports many modules that stay in memory
for mod in list(sys.modules.keys()):
    if mod not in ('gc', 'sys', 'engine_main', 'engine', 'doom', '__main__',
                   'builtins', 'micropython', '_thread'):
        del sys.modules[mod]

gc.collect()
engine.fps_limit(30)

import doom

gc.collect()

if sys.platform != "linux":
    doom.init("doom1.wad")
else:
    try:
        import os
        os.stat("Games/Doom/assets/doom1.wad")
        doom.init("Games/Doom/assets/doom1.wad")
    except OSError:
        doom.init("assets/doom1.wad")

doom.run_loop()
doom.deinit()
