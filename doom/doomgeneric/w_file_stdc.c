/*
 * w_file_stdc.c — WAD file I/O
 *
 * On Unix: standard fopen/fread/fseek (original doomgeneric code).
 * On ARM (RP2350): MicroPython stream API since stdio doesn't work.
 */

#include <stdio.h>
#include <string.h>

#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

#ifdef __arm__
/* ---- RP2350: MicroPython stream-based WAD I/O ---- */

#include "py/runtime.h"
#include "py/stream.h"

/* The Python file object is passed in from doom_core.c */
extern mp_obj_t doom_wad_file_obj;
extern size_t doom_wad_file_size;

typedef struct
{
    wad_file_t wad;
    mp_obj_t file_obj;
    const mp_stream_p_t *stream_p;
} mp_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *W_MP_OpenFile(char *path)
{
    mp_wad_file_t *result;

    if (doom_wad_file_obj == MP_OBJ_NULL)
        return NULL;

    result = Z_Malloc(sizeof(mp_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = doom_wad_file_size;
    result->file_obj = doom_wad_file_obj;
    result->stream_p = mp_get_stream(doom_wad_file_obj);

    return &result->wad;
}

static void W_MP_CloseFile(wad_file_t *wad)
{
    /* Don't close the file — Python owns it */
    Z_Free(wad);
}

static size_t W_MP_Read(wad_file_t *wad, unsigned int offset,
                        void *buffer, size_t buffer_len)
{
    mp_wad_file_t *mp_wad = (mp_wad_file_t *)wad;
    int errcode = 0;

    /* Seek to offset */
    struct mp_stream_seek_t seek_s = { .offset = offset, .whence = 0 };
    mp_wad->stream_p->ioctl(mp_wad->file_obj, MP_STREAM_SEEK,
                            (mp_uint_t)(uintptr_t)&seek_s, &errcode);

    /* Read data */
    size_t got = mp_wad->stream_p->read(mp_wad->file_obj, buffer, buffer_len, &errcode);
    return got;
}

wad_file_class_t stdc_wad_file =
{
    W_MP_OpenFile,
    W_MP_CloseFile,
    W_MP_Read,
};

#else
/* ---- Unix: standard stdio WAD I/O ---- */

typedef struct
{
    wad_file_t wad;
    FILE *fstream;
} stdc_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *W_StdC_OpenFile(char *path)
{
    stdc_wad_file_t *result;
    FILE *fstream;

    fstream = fopen(path, "rb");

    if (fstream == NULL)
    {
        return NULL;
    }

    result = Z_Malloc(sizeof(stdc_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = M_FileLength(fstream);
    result->fstream = fstream;

    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad)
{
    stdc_wad_file_t *stdc_wad;

    stdc_wad = (stdc_wad_file_t *) wad;

    fclose(stdc_wad->fstream);
    Z_Free(stdc_wad);
}

static size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                   void *buffer, size_t buffer_len)
{
    stdc_wad_file_t *stdc_wad;
    size_t result;

    stdc_wad = (stdc_wad_file_t *) wad;

    fseek(stdc_wad->fstream, offset, SEEK_SET);

    result = fread(buffer, 1, buffer_len, stdc_wad->fstream);

    return result;
}

wad_file_class_t stdc_wad_file =
{
    W_StdC_OpenFile,
    W_StdC_CloseFile,
    W_StdC_Read,
};

#endif /* __arm__ */
