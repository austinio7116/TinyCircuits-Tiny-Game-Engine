/*
 * w_file_xip.c — XIP flash-mapped WAD file access for Thumby Color.
 *
 * The WAD is stored at a known flash offset and accessed via XIP
 * memory mapping. Zero copy, zero RAM overhead for lump data.
 */

#ifdef __arm__

#include <string.h>
#include <stdio.h>
#include "w_file.h"
#include "z_zone.h"

/* WAD at 2MB flash offset — firmware is ~1MB, WAD ends at ~6.2MB, FS at 8MB */
#define WAD_FLASH_OFFSET   0x200000
#define WAD_XIP_BASE       (0x10000000 + WAD_FLASH_OFFSET)

extern wad_file_class_t xip_wad_file;

typedef struct {
    wad_file_t wad;
} xip_wad_file_t;

static wad_file_t *W_XIP_OpenFile(char *path)
{
    const uint8_t *flash_ptr = (const uint8_t *)WAD_XIP_BASE;

    /* Validate WAD magic at flash address */
    if (memcmp(flash_ptr, "IWAD", 4) != 0 &&
        memcmp(flash_ptr, "PWAD", 4) != 0) {
        printf("XIP: No WAD at 0x%08X\n", WAD_XIP_BASE);
        return NULL;
    }

    /* Read WAD length from header */
    uint32_t numlumps = *(const uint32_t *)(flash_ptr + 4);
    uint32_t infotableofs = *(const uint32_t *)(flash_ptr + 8);
    uint32_t wad_len = infotableofs + numlumps * 16;

    xip_wad_file_t *result = Z_Malloc(sizeof(xip_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &xip_wad_file;
    result->wad.mapped = (byte *)flash_ptr;  /* Memory-mapped pointer */
    result->wad.length = wad_len;

    printf("XIP: WAD mapped at 0x%08X, %u lumps, %uKB\n",
           WAD_XIP_BASE, numlumps, wad_len / 1024);

    return &result->wad;
}

static void W_XIP_CloseFile(wad_file_t *wad)
{
    Z_Free(wad);
}

static size_t W_XIP_Read(wad_file_t *wad, unsigned int offset,
                         void *buffer, size_t buffer_len)
{
    /* Direct memcpy from flash — used for WAD directory parsing */
    memcpy(buffer, (const uint8_t *)WAD_XIP_BASE + offset, buffer_len);
    return buffer_len;
}

wad_file_class_t xip_wad_file = {
    W_XIP_OpenFile,
    W_XIP_CloseFile,
    W_XIP_Read,
};

#endif /* __arm__ */
