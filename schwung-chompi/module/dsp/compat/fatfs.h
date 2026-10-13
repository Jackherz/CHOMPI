/**
 * compat/fatfs.h — FatFS-shaped file API backed by POSIX, for the Move port.
 *
 * WHY: CHOMPI runs every file operation on a low-priority SD callback and
 * streams samples to the audio thread through FIFOs. That is exactly the
 * shape Schwung demands (no file I/O on the audio callback), so the port
 * keeps CHOMPI's structure verbatim and only swaps the executor:
 *
 *     firmware:  SDCallback (Daisy low-priority callback)  -> f_read()
 *     port:      io_worker  (SCHED_OTHER, cores 0-2)       -> pread()/read()
 *
 * f_size()/f_tell() therefore read CACHED values maintained by the I/O thread
 * instead of issuing lseek() — SampleReader.h calls them on the audio thread
 * (SetStartPoint, CheckDoubleSpeed), so they must never syscall.
 *
 * This is not a general FatFS implementation. It covers exactly the surface
 * the ported engine uses; FileCopier/OptionsManager are replaced by the
 * module's own sample-library handling (see PORTING.md).
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

#include "wav_format.h" // daisy::WAV_FormatTypeDef + kWavFile* + WAVE_FORMAT_* (vendored libDaisy)

typedef int FRESULT;
typedef std::uint32_t FSIZE_t;
typedef std::uint32_t UINT;
typedef std::uint64_t QWORD;

enum
{
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
};

/* FatFS open flags (only the ones the engine uses) */
/* The port's WAV translator (port/sample_view.h): a file the engine cannot read
 * as-is gets one of these, and f_read/f_lseek/f_size speak in converted bytes. */
namespace chompi
{
struct SampleView;
}

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10
#define FA_OPEN_APPEND 0x30

/** FatFS file object. The port keeps the descriptor plus the cached
 *  size/position the audio thread is allowed to read. */
typedef struct
{
    /* NOTE: these are default-initialised on purpose. std::atomic's default
     * constructor leaves the value indeterminate before C++20, and
     * FileSampleReader holds a FIL by value with no initialiser — so a
     * zeroed-out FIL is the difference between "opens a file" and "seeks to
     * a garbage offset". */
    int                  fd      = -1;
    std::atomic<int64_t> size{0};
    std::atomic<int64_t> tell{0};
    std::atomic<bool>    in_use{false};

    /* FatFS exposes the directory entry through FIL::obj. The engine's
     * TRUNCATE path clears obj.objsize; keep a stand-in so the vendored
     * FileStreamingManager.cpp stays unmodified. */
    struct
    {
        std::uint32_t objsize = 0;
    } obj;

    /* When the file is not in the engine's native layout (16-bit stereo at
     * byte 44), this holds the converter: see port/sample_view.h. `size`/`tell`
     * above are then VIRTUAL (converted) bytes, which is all the engine ever
     * sees. Freed by f_close()/re-open; nullptr for every native file. */
    chompi::SampleView* sample_view = nullptr;
} FIL;

typedef struct
{
    FSIZE_t fsize;
    int     fattrib;
} FILINFO;

#ifdef __cplusplus
extern "C"
{
#endif

FRESULT f_open(FIL* fp, const char* path, unsigned char mode);
FRESULT f_close(FIL* fp);
FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek(FIL* fp, FSIZE_t ofs);
FRESULT f_sync(FIL* fp);
FRESULT f_truncate(FIL* fp);
FRESULT f_stat(const char* path, FILINFO* fno);
FRESULT f_unlink(const char* path);

/** Cached accessors — no syscall, safe from the audio callback. */
FSIZE_t f_size(FIL* fp);
FSIZE_t f_tell(FIL* fp);
int     f_eof(FIL* fp);

#ifdef __cplusplus
}
#endif
