#ifndef DOOM_CORE_H
#define DOOM_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Initialize DOOM with a WAD file path (Unix) or file object (ARM).
 * Returns 0 on success, negative on error. */
int doom_core_init(const char *wad_path);

/* Set WAD file object for ARM stream I/O (call before init) */
void doom_core_set_wad_file(void *file_obj, size_t size);

/* Run the DOOM main loop entirely in C until MENU pressed. Returns frame count. */
int doom_core_run_loop(void);

/* Free all dynamically allocated DOOM buffers. */
void doom_core_deinit(void);

/* Loading progress bar (0-100%) */
void doom_loading_progress(int percent);

/* Audio: get next sample for engine mixing. Returns float -1.0 to 1.0. */
float doom_get_audio_sample(void);

#endif /* DOOM_CORE_H */
