#line 2 "engine_resource_manager.c"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "engine_resource_manager.h"
#include "debug/debug_print.h"
#include "py/obj.h"
#include "py/misc.h"
#include "utility/engine_file.h"

#include "fault/engine_trace_portable.h"

#if defined(__EMSCRIPTEN__) || defined(__unix__)
    #define FLASH_PAGE_SIZE 256
    #define FLASH_SECTOR_SIZE 4096
    #define FLASH_RESOURCE_SPACE_BASE 0
    #define FLASH_RESOURCE_SPACE_SIZE 2 * 1024 * 1024
    #define XIP_BASE 0

    uint8_t *scratch_space = NULL;
#else
    #include "pico/stdlib.h"
    #include "hardware/flash.h"
    #include "hardware/sync.h"

    #ifdef THUMBYONE_SLOT_MODE
        /* ThumbyOne slot build: the SDK's flash_range_erase /
         * flash_range_program reset QMI ATRANS (which we need for
         * the chained slot's self-view of flash) AND clobber the
         * fast QPI continuous-read XIP config that boot2 set up.
         * Without re-applying both after every flash op, the next
         * XIP fetch either reads from the wrong physical address
         * or drops to single-SPI speed — both manifest as "things
         * get slower and slower as the game caches more textures".
         *
         * thumbyone_xip_fast_setup() lives in ThumbyOne/common/
         * thumbyone_handoff.c (linked into every slot image by the
         * parent CMake). */
        #include "hardware/structs/qmi.h"

        /* Derive the flash resource scratch region from the canonical
         * ThumbyOne layout instead of hardcoding it. slot_layout.h is
         * force-included via the board's mpconfigboard.h, so these
         * macros are already in scope; the explicit include documents
         * the dependency and keeps this robust if that chain changes.
         * Scratch is the top THUMBYONE_MPY_SCRATCH_SIZE bytes of the
         * MPY partition (flash offset, XIP_BASE added at read time). */
        #include "slot_layout.h"
        #define FLASH_RESOURCE_SPACE_BASE  THUMBYONE_MPY_SCRATCH_OFFSET
        #define FLASH_RESOURCE_SPACE_SIZE  THUMBYONE_MPY_SCRATCH_SIZE

        /* FAT-backed overflow scratch. The 256 KB static scratch above
         * can't hold a large game's assets (Monstra needs ~2 MB of
         * textures + WaveSound audio, all stored in flash scratch). When
         * an asset won't fit the static region, we reserve a CONTIGUOUS
         * file from the shared FAT's free space, map it as a second
         * scratch region via XIP, and return it when the game restarts.
         * Borrowed only while a big game runs — nothing is pre-allocated
         * for everyone else. */
        #include "lib/fatfs/ff.h"
        #include "extmod/vfs_fat.h"

        extern void thumbyone_xip_fast_setup(void);
        static inline void thumbyone_save_atrans(uint32_t out[4]) {
            out[0] = qmi_hw->atrans[0];
            out[1] = qmi_hw->atrans[1];
            out[2] = qmi_hw->atrans[2];
            out[3] = qmi_hw->atrans[3];
        }
        static inline void thumbyone_restore_atrans(const uint32_t in[4]) {
            qmi_hw->atrans[0] = in[0];
            qmi_hw->atrans[1] = in[1];
            qmi_hw->atrans[2] = in[2];
            qmi_hw->atrans[3] = in[3];
            thumbyone_xip_fast_setup();
        }
    #endif

    // Scratch region for non-in-RAM resources (TextureResource,
    // MeshResource, etc.) stored directly in flash. Must sit OUTSIDE
    // the firmware image and the MicroPython filesystem, because
    // engine_resource_reset() erases sectors here on every engine
    // init. Default layout (standalone Thumby Color MPY firmware
    // occupying the whole flash): scratch starts at 1 MiB in, runs
    // up to the start of the MicroPython littlefs/FAT partition.
    //
    // Multi-slot builds (ThumbyOne) override these via -D flags in
    // the port CMake: the slot's firmware + FS share the flash with
    // other slots, so the defaults here would overwrite siblings.
    // PARTITION: | FIRMWARE | SCRATCH | FILESYSTEM |
    #ifndef FLASH_RESOURCE_SPACE_BASE
        #define FLASH_RESOURCE_SPACE_BASE (1 * 1024 * 1024)
    #endif

    #ifndef FLASH_RESOURCE_SPACE_SIZE
        #define FLASH_RESOURCE_SPACE_SIZE (PICO_FLASH_SIZE_BYTES - (MICROPY_HW_FLASH_STORAGE_BYTES + FLASH_RESOURCE_SPACE_BASE))
    #endif
#endif


// Intermediate buffer to hold data read from flash before
// programming it to a contigious flash area
uint8_t page_prog[FLASH_PAGE_SIZE];
uint16_t page_prog_index = 0;
uint32_t page_prog_count = 0;

// How many pages (not sectors), that have been used so far
uint32_t used_pages_count = 0;


// These are used for tracking where we are storing
// data and where the data is stored inside that
// location
uint8_t *current_storing_location = NULL;
uint32_t index_in_storing_location = 0;
bool storing_in_ram = false;


#if defined(__arm__)
// Active scratch region for the bump allocator. Defaults to the static
// scratch region; under THUMBYONE_SLOT_MODE it switches to a FAT-backed
// overflow region once an asset won't fit the static region.
static uint32_t scratch_region_base = FLASH_RESOURCE_SPACE_BASE;  // flash offset
static uint32_t scratch_region_size = FLASH_RESOURCE_SPACE_SIZE;
#endif


#ifdef THUMBYONE_SLOT_MODE
// FAT-backed overflow scratch. Rather than grabbing all free space up
// front (which left a multi-MB file orphaned on a mid-game power-off and
// starved in-play saves), the scratch GROWS ON DEMAND: each time the
// current region fills, we reserve one more fixed-size contiguous chunk
// from the FAT — so total borrowed space tracks what the game actually
// uses. Chunks are files "/.engine_scratch0", "1", ... released together.
#define FAT_SCRATCH_PREFIX       "/.engine_scratch"
#define FAT_SCRATCH_CHUNK        (1024u * 1024u)        // grow a MiB at a time
#define FAT_SCRATCH_MAX_CHUNKS   32u                    // hard cap (32 MiB)
#define FAT_SCRATCH_SAVE_RESERVE (512u * 1024u)         // keep free for in-play saves

// How many chunks we've reserved this game session.
static uint32_t fat_chunk_count = 0;

// Build "/.engine_scratch<idx>" into buf (>= 20 bytes). idx < 100.
static void fat_scratch_path(uint32_t idx, char *buf){
    const char *p = FAT_SCRATCH_PREFIX;
    char *d = buf;
    while(*p){ *d++ = *p++; }
    if(idx >= 10u){ *d++ = (char)('0' + (idx / 10u)); }
    *d++ = (char)('0' + (idx % 10u));
    *d = '\0';
}

// Delete every reserved chunk file, returning its space to the drive.
// Safe to call when nothing was reserved — also clears stale chunks (and
// the pre-chunking legacy name) left behind by a crash or power-off.
static void fat_scratch_release(void){
    if(mp_vfs_fat_get_mounted() == NULL){
        return;
    }
    f_unlink(FAT_SCRATCH_PREFIX);   // legacy single-file name from earlier builds
    char path[20];
    for(uint32_t i = 0; i < FAT_SCRATCH_MAX_CHUNKS; i++){
        fat_scratch_path(i, path);
        if(f_unlink(path) != FR_OK){
            break;   // first absent index — no further chunks exist
        }
    }
    fat_chunk_count = 0;
}

// Current free space on the shared FAT, in KB (0 if unavailable). Used to
// give an out-of-space error that reports the actionable number.
static uint32_t fat_scratch_free_kb(void){
    fs_user_mount_t *vfs = mp_vfs_fat_get_mounted();
    if(vfs == NULL){
        return 0;
    }
    FATFS *fs = &vfs->fatfs;
    DWORD free_clst; FATFS *unused;
    if(f_getfree("", &free_clst, &unused) != FR_OK){
        return 0;
    }
    uint64_t free_bytes = (uint64_t)free_clst * (uint32_t)fs->csize * 512u;
    return (uint32_t)(free_bytes / 1024u);
}

// Reserve one more CONTIGUOUS chunk (index idx) of at least min_bytes — an
// asset can't span chunks, so a chunk is max(FAT_SCRATCH_CHUNK, asset). On
// success *out_base / *out_size give a 4 KB-aligned flash region (offset,
// not XIP). Returns false if no big-enough contiguous free block exists,
// so the caller can raise a clear "free up space" error.
static bool fat_scratch_reserve_chunk(uint32_t idx, uint32_t min_bytes,
                                      uint32_t *out_base, uint32_t *out_size){
    fs_user_mount_t *vfs = mp_vfs_fat_get_mounted();
    if(vfs == NULL){
        return false;
    }
    FATFS *fs = &vfs->fatfs;

    char path[20];
    fat_scratch_path(idx, path);
    f_unlink(path);   // clear any stale file at this index

    FIL fp;
    if(f_open(&fp, path, FA_CREATE_ALWAYS | FA_WRITE | FA_READ) != FR_OK){
        return false;
    }

    uint32_t cluster_bytes = (uint32_t)fs->csize * 512u;
    uint32_t need = min_bytes + 2u * FLASH_SECTOR_SIZE;   // alignment loses up to ~2 sectors

    // One chunk, but never smaller than this asset needs.
    uint32_t want = (FAT_SCRATCH_CHUNK > need) ? FAT_SCRATCH_CHUNK : need;
    want = (want + cluster_bytes - 1u) / cluster_bytes * cluster_bytes;  // whole clusters

    // Don't exceed free space; keep a little for in-play saves.
    DWORD free_clst; FATFS *unused;
    if(f_getfree("", &free_clst, &unused) == FR_OK){
        uint64_t free_bytes = (uint64_t)free_clst * cluster_bytes;
        uint32_t avail = (free_bytes > FAT_SCRATCH_SAVE_RESERVE)
                       ? (uint32_t)(free_bytes - FAT_SCRATCH_SAVE_RESERVE) : 0u;
        if(want > avail){
            want = avail / cluster_bytes * cluster_bytes;
        }
    }

    // f_expand needs a CONTIGUOUS run. On a fragmented drive the request
    // can fail with free space still available, so shrink and retry to
    // find the largest contiguous block that still covers need.
    FRESULT r = FR_DENIED;
    while(want >= need){
        r = f_expand(&fp, (FSIZE_t)want, 1);   // opt=1: allocate contiguous now
        if(r == FR_OK){
            break;
        }
        uint32_t next = (want / 2u) / cluster_bytes * cluster_bytes;
        if(next == 0u || next == want){
            break;
        }
        want = next;
    }
    if(r != FR_OK){
        f_close(&fp);
        f_unlink(path);
        return false;
    }

    // Start cluster -> absolute flash offset.
    DWORD sclust = fp.obj.sclust;
    LBA_t lba = fs->database + (LBA_t)(sclust - 2u) * fs->csize;
    uint32_t flash_off = (uint32_t)THUMBYONE_FAT_OFFSET + (uint32_t)((uint64_t)lba * 512u);
    uint32_t flash_end = flash_off + want;

    f_close(&fp);   // keep the file so its clusters stay reserved while we use them

    // Use only the 4 KB-aligned interior. FAT clusters are 1 KB but flash
    // erases in 4 KB sectors, so the leading/trailing partial sectors may
    // share a sector with a neighbouring file — never erase those.
    uint32_t abase = (flash_off + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
    uint32_t aend  = flash_end & ~(FLASH_SECTOR_SIZE - 1u);
    if(aend <= abase || (aend - abase) < min_bytes){
        f_unlink(path);
        return false;
    }

    *out_base = abase;
    *out_size = aend - abase;
    return true;
}
#endif  // THUMBYONE_SLOT_MODE


void engine_resource_init(){
    #if defined(__unix__) || defined(__EMSCRIPTEN__)
        if(scratch_space == NULL) scratch_space = malloc(FLASH_RESOURCE_SPACE_SIZE);
    #else
        #ifdef THUMBYONE_SLOT_MODE
            // First-run cleanup: a previous game (or crash) may have left
            // a /.engine_scratch file holding borrowed FAT space. Clear it
            // so a small game launched after a big one frees that space.
            fat_scratch_release();
        #endif
    #endif
}


void engine_resource_reset(){
    ENGINE_PRINTF("EngineResourceManager: Resetting...\n");
    #if defined(__arm__)
        page_prog_index = 0;
        page_prog_count = 0;
        used_pages_count = 0;
        current_storing_location = NULL;
        index_in_storing_location = 0;
        storing_in_ram = false;
        // Bump allocator starts each game in the static scratch region.
        scratch_region_base = FLASH_RESOURCE_SPACE_BASE;
        scratch_region_size = FLASH_RESOURCE_SPACE_SIZE;
    #endif
    #ifdef THUMBYONE_SLOT_MODE
        // Return all FAT chunks borrowed by the previous game (and clear
        // stale chunks from a crash or power-off). Each game re-grows its
        // scratch on demand as it loads, so borrowed space tracks actual
        // use. fat_scratch_release() resets fat_chunk_count to 0.
        fat_scratch_release();
    #endif
}


mp_obj_t engine_resource_get_space_bytearray(uint32_t space_size, bool fast_space){
    mp_obj_array_t *array = m_new_obj(mp_obj_array_t);
    array->base.type = &mp_type_bytearray;
    array->typecode = BYTEARRAY_TYPECODE;
    array->free = 0;
    array->len = space_size;

    if(fast_space){
        array->items = m_new(byte, array->len);
        memset(array->items, 0, array->len);
    }else{
        // How many flash pages will be needed to fit 'space_size' data? 
        // Pages are 256 bytes and data must be written in that page size:
        // https://www.raspberrypi.com/documentation/pico-sdk/hardware.html#rpip8ee511575881aa0f3936
        uint32_t required_pages_count = (uint32_t)ceilf(((float)space_size)/FLASH_PAGE_SIZE);

#ifdef THUMBYONE_SLOT_MODE
        // If this asset won't fit the current region — the static scratch,
        // or the latest FAT chunk — grow by reserving ONE more contiguous
        // chunk from the FAT, sized to what's needed (not all free space).
        // An asset must live in one contiguous region, so we switch
        // wholesale; any tail of the previous region is left unused.
        {
            uint32_t region_capacity_pages = scratch_region_size / FLASH_PAGE_SIZE;
            if(used_pages_count + required_pages_count > region_capacity_pages){
                uint32_t chunk_base = 0, chunk_size = 0;
                if(fat_chunk_count < FAT_SCRATCH_MAX_CHUNKS &&
                   fat_scratch_reserve_chunk(fat_chunk_count, space_size, &chunk_base, &chunk_size)){
                    scratch_region_base = chunk_base;
                    scratch_region_size = chunk_size;
                    used_pages_count = 0;
                    fat_chunk_count++;
                }
                // If reservation failed (no contiguous space), the bounds
                // check below raises the clear "free up space" error.
            }
        }
#endif

#if defined(__arm__)
        const uint32_t region_base = scratch_region_base;
        const uint32_t region_size = scratch_region_size;
#else
        const uint32_t region_base = FLASH_RESOURCE_SPACE_BASE;
        const uint32_t region_size = FLASH_RESOURCE_SPACE_SIZE;
#endif

        // Based on how many pages have been used so far, how many sectors have
        // already been erased? This will be used to find the base offset of sectors
        // to erase if the extra pages end up in new sectors. Sectors are 4096 bytes
        // and must be erased in that sector size:
        // https://www.raspberrypi.com/documentation/pico-sdk/hardware.html#rpip8ee511575881aa0f3936
        uint32_t already_erased_sectors_count = (uint32_t)ceilf(((float)(used_pages_count*FLASH_PAGE_SIZE))/FLASH_SECTOR_SIZE);

        // Based on how many pages have been used and how many more are going to be used,
        // how many sectors should be erased
        uint32_t total_erase_sector_count = (uint32_t)ceilf(((float)((required_pages_count+used_pages_count)*FLASH_PAGE_SIZE))/FLASH_SECTOR_SIZE);

        uint32_t additional_sectors_to_erase_count = total_erase_sector_count - already_erased_sectors_count;

        // Get the offset where sector erasing will start and
        // how many sectors, in bytes to erase, then erase them
        uint32_t sectors_to_erase_offset = already_erased_sectors_count*FLASH_SECTOR_SIZE;
        uint32_t sectors_to_erase_size   = additional_sectors_to_erase_count*FLASH_SECTOR_SIZE;

        // Before erasing everything for the requested size,
        // check if we're going to be erasing addresses out of
        // bounds, stop everything if that is going to happen
        // (will lose filesystem otherwise!)
        const uint32_t erase_size = sectors_to_erase_size;
        const uint32_t erase_start = region_base + sectors_to_erase_offset;
        const uint32_t erase_end = erase_start + erase_size;

        if(erase_end > region_base + region_size){
#ifdef THUMBYONE_SLOT_MODE
            // Static scratch + whatever we could borrow from the FAT both
            // exhausted (or no contiguous FAT space was available). Tell
            // the player plainly instead of dumping flash addresses.
            mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT(
                "Not enough space on the Thumby drive to load this game's graphics and sound. Only %ld KB is free - delete some games or files and reboot."),
                (long)fat_scratch_free_kb());
#else
            mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("EngineResourceManager: ERROR: Scratch space is going to overflow! Too many assets loaded! Scratch space is %ld bytes but the asset requires erasing %ld bytes from %ld to %ld"), region_size, erase_size, erase_start, erase_end);
#endif
        }
        
        #if defined(__arm__)
            // Need to disable interrupts when texture resources are created:
            // https://github.com/raspberrypi/pico-examples/issues/34#issuecomment-1369267917
            // otherwise hangs forever
            uint32_t paused_interrupts = save_and_disable_interrupts();
            #ifdef THUMBYONE_SLOT_MODE
                uint32_t saved_atrans[4]; thumbyone_save_atrans(saved_atrans);
            #endif
            flash_range_erase(erase_start, erase_size);
            #ifdef THUMBYONE_SLOT_MODE
                thumbyone_restore_atrans(saved_atrans);
            #endif
            restore_interrupts(paused_interrupts);

            // Stored in contiguous flash location
            array->items = (uint8_t*)(XIP_BASE + region_base + (used_pages_count*FLASH_PAGE_SIZE));
        #else
            array->items = scratch_space + XIP_BASE + region_base + (used_pages_count*FLASH_PAGE_SIZE);
        #endif

        used_pages_count += required_pages_count;
    }

    return array;
}


void engine_resource_start_storing(mp_obj_t bytearray, bool in_ram){
    current_storing_location = ENGINE_BYTEARRAY_OBJ_TO_DATA(bytearray);
    index_in_storing_location = 0;
    storing_in_ram = in_ram;

    // When using flash, need a way to track how many pages
    // in this storing operation have been stored, use this
    // to track
    page_prog_index = 0;
    page_prog_count = 0;
}


void engine_resource_store_u8(uint8_t to_store){
    if(storing_in_ram){
        // Convert to u16 array
        uint8_t *u8_current_storing_location = (uint8_t*)current_storing_location;
        u8_current_storing_location[index_in_storing_location] = to_store;
    }else{
        uint8_t *u8_page_prog = (uint8_t*)page_prog;

        // Store the 'to_byte' byte in a buffer in ram for now
        u8_page_prog[page_prog_index] = to_store;

        // Once buffer is full, write it to flash and
        // reset indices to start filling again
        page_prog_index++;
        if(page_prog_index >= FLASH_PAGE_SIZE){

            #if defined(__arm__)
                uint32_t address_offset = ((uint32_t)current_storing_location) - XIP_BASE;
                uint32_t paused_interrupts = save_and_disable_interrupts();
                #ifdef THUMBYONE_SLOT_MODE
                    uint32_t saved_atrans[4]; thumbyone_save_atrans(saved_atrans);
                #endif
                flash_range_program(address_offset + (page_prog_count*FLASH_PAGE_SIZE), page_prog, FLASH_PAGE_SIZE);
                #ifdef THUMBYONE_SLOT_MODE
                    thumbyone_restore_atrans(saved_atrans);
                #endif
                restore_interrupts(paused_interrupts);
            #else
                memcpy(current_storing_location + (page_prog_count*FLASH_PAGE_SIZE), page_prog, FLASH_PAGE_SIZE);
            #endif

            page_prog_index = 0;
            page_prog_count++;
        }
    }
    
    index_in_storing_location++;
}


void engine_resource_store_u16(uint16_t to_store){
    if(storing_in_ram){
        // Convert to u16 array
        uint16_t *u16_current_storing_location = (uint16_t*)current_storing_location;
        u16_current_storing_location[index_in_storing_location] = to_store;
    }else{
        uint16_t *u16_page_prog = (uint16_t*)page_prog;

        // Store the 'to_byte' byte in a buffer in ram for now
        u16_page_prog[page_prog_index] = to_store;

        // Once buffer is full, write it to flash and
        // reset indices to start filling again
        page_prog_index++;
        if(page_prog_index >= FLASH_PAGE_SIZE/2){
            #if defined(__arm__)
                uint32_t address_offset = ((uint32_t)current_storing_location) - XIP_BASE;
                uint32_t paused_interrupts = save_and_disable_interrupts();
                #ifdef THUMBYONE_SLOT_MODE
                    uint32_t saved_atrans[4]; thumbyone_save_atrans(saved_atrans);
                #endif
                flash_range_program(address_offset + (page_prog_count*FLASH_PAGE_SIZE), page_prog, FLASH_PAGE_SIZE);
                #ifdef THUMBYONE_SLOT_MODE
                    thumbyone_restore_atrans(saved_atrans);
                #endif
                restore_interrupts(paused_interrupts);
            #else
                memcpy(current_storing_location + (page_prog_count*FLASH_PAGE_SIZE), page_prog, FLASH_PAGE_SIZE);
            #endif

            page_prog_index = 0;
            page_prog_count++;
        }
    }
    
    index_in_storing_location++;
}


void engine_resource_stop_storing(){
    if(page_prog_index != 0){
        #if defined(__arm__)
            uint32_t address_offset = ((uint32_t)current_storing_location) - XIP_BASE;
            uint32_t paused_interrupts = save_and_disable_interrupts();
            #ifdef THUMBYONE_SLOT_MODE
                uint32_t saved_atrans[4]; thumbyone_save_atrans(saved_atrans);
            #endif
            flash_range_program(address_offset + (page_prog_count*FLASH_PAGE_SIZE), page_prog, FLASH_PAGE_SIZE);
            #ifdef THUMBYONE_SLOT_MODE
                thumbyone_restore_atrans(saved_atrans);
            #endif
            restore_interrupts(paused_interrupts);
        #else
            memcpy(current_storing_location + (page_prog_count*FLASH_PAGE_SIZE), page_prog, FLASH_PAGE_SIZE);
        #endif
    }
}
