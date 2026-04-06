#ifndef DOOM_CACHE_H
#define DOOM_CACHE_H

#include <stdint.h>
#include <stddef.h>

/* Load doom_cache.bin from a file path or MicroPython file object.
 * Returns 0 on success. Cache data is stored in a single malloc'd block. */
int doom_cache_load(const char *path);
int doom_cache_load_mp(void *file_obj, size_t file_size);

/* Look up a lump by name in the cache.
 * Returns pointer to data and sets *size, or NULL if not cached. */
const void *doom_cache_find(const char *name, int *size);

/* Look up by lump index (using the WAD's lumpinfo name) */
const void *doom_cache_find_by_lumpinfo(int lumpnum, int *size);

#endif
