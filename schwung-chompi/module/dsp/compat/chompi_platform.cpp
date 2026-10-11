/**
 * compat/chompi_platform.cpp — Linux implementations of the compat surface:
 *   - daisy::System::GetNow()
 *   - the POSIX-backed FatFS shim (compat/fatfs.h)
 *
 * All of it runs on the I/O worker thread or the loader thread. Nothing in
 * this file may be called from the SPI/audio callback (the one exception is
 * f_size/f_tell, which are pure atomic reads of cached fields).
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>

#include "daisy.h"
#include "fatfs.h"
#include "chompi_port.h"

// ---------------------------------------------------------------- daisy::System

namespace daisy
{
uint32_t System::GetNow()
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    const auto ms = duration_cast<milliseconds>(steady_clock::now() - start).count();
    return static_cast<uint32_t>(ms); // deliberate wrap-around, as on hardware
}
} // namespace daisy

// ---------------------------------------------------------------- FatFS shim

static FRESULT errno_to_fresult(int e)
{
    switch(e)
    {
        case 0: return FR_OK;
        case ENOENT: return FR_NO_FILE;
        case EACCES:
        case EPERM: return FR_DENIED;
        case EEXIST: return FR_EXIST;
        case ENOSPC: return FR_DISK_ERR;
        case EINVAL: return FR_INVALID_PARAMETER;
        default: return FR_DISK_ERR;
    }
}

extern "C"
{
    FRESULT f_open(FIL* fp, const char* path, unsigned char mode)
    {
        if(!fp || !path)
            return FR_INVALID_PARAMETER;

        int flags = 0;
        const bool want_read  = (mode & FA_READ) != 0;
        const bool want_write = (mode & FA_WRITE) != 0;

        if(want_write)
            flags = O_RDWR;
        else if(want_read)
            flags = O_RDONLY;
        else
            flags = O_RDWR;

        /* PORT PATCH P11: never create or truncate a file we do not own.
         *
         * The engine opens samples FA_OPEN_ALWAYS, and in FatFS that creates the
         * file when it is missing -- CHOMPI's own behaviour, correct for its own
         * SD card. The port can point a slot at any file on the device, though,
         * and that create would land in the user's sample library: play a slot
         * whose assigned file has since been deleted and a 0-byte stub appears
         * where the user's sample used to be. Inside the module's own directory
         * the firmware semantics stand (a missing convention file becomes a
         * placeholder, which is what `debug_files` and the slot scan expect);
         * outside it a missing file is simply missing, and the slot stays
         * silent. FA_CREATE_ALWAYS is used for the recorder's own files, which
         * live in that same directory -- but gate it too, so no path can ever
         * truncate something the module does not own. */
        const bool may_write_new = chompi::IsManagedPath(path);
        if(mode & FA_CREATE_ALWAYS)
            flags |= may_write_new ? (O_CREAT | O_TRUNC) : 0;
        else if(mode & FA_OPEN_ALWAYS)
            flags |= may_write_new ? O_CREAT : 0;

        /* PORT P12: re-opening a file object that is already open must not
         * leak the old descriptor.
         *
         * The engine re-opens a reader's FIL directly, and it skips the CLOSE
         * when the file it holds is empty (`f_size() == 0`, in
         * FileStreamingManager's CLOSE case and in SampleReader::OpenFile).
         * FatFS is a library, so that cost the firmware nothing; here it is a
         * kernel descriptor, and every slot that has no file -- every key
         * pressed for an empty slot -- leaked one. The descriptor count is
         * what eventually makes f_open fail, and a failed open used to silence
         * the whole voice (see FileOpenFailed). */
        if(fp->in_use.load(std::memory_order_relaxed))
        {
            ::close(fp->fd);
            fp->in_use.store(false, std::memory_order_relaxed);
        }

        const int fd = ::open(path, flags, 0644);
        if(fd < 0)
        {
            chompi::g_open_fail_count++;
            snprintf(chompi::g_last_open_fail, sizeof(chompi::g_last_open_fail), "%s", path);
            chompi::g_last_open_errno = errno;
            return errno_to_fresult(errno);
        }

        struct stat st;
        if(fstat(fd, &st) != 0)
        {
            ::close(fd);
            return FR_DISK_ERR;
        }

        fp->fd = fd;
        fp->size.store(static_cast<int64_t>(st.st_size), std::memory_order_relaxed);
        fp->tell.store(0, std::memory_order_relaxed);
        fp->in_use.store(true, std::memory_order_relaxed);
        return FR_OK;
    }

    FRESULT f_close(FIL* fp)
    {
        if(!fp || !fp->in_use.load(std::memory_order_relaxed))
            return FR_INVALID_OBJECT;
        const int ret = ::close(fp->fd);
        fp->in_use.store(false, std::memory_order_relaxed);
        return ret == 0 ? FR_OK : FR_DISK_ERR;
    }

    FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br)
    {
        if(br)
            *br = 0;
        if(!fp || !fp->in_use.load(std::memory_order_relaxed))
            return FR_INVALID_OBJECT;

        const ssize_t got = ::read(fp->fd, buff, btr);
        if(got < 0)
            return errno_to_fresult(errno);

        if(br)
            *br = static_cast<UINT>(got);
        fp->tell.fetch_add(got, std::memory_order_relaxed);
        return FR_OK;
    }

    FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
    {
        if(bw)
            *bw = 0;
        if(!fp || !fp->in_use.load(std::memory_order_relaxed))
            return FR_INVALID_OBJECT;

        const ssize_t put = ::write(fp->fd, buff, btw);
        if(put < 0)
            return errno_to_fresult(errno);

        if(bw)
            *bw = static_cast<UINT>(put);
        const int64_t tell = fp->tell.fetch_add(put, std::memory_order_relaxed) + put;
        if(tell > fp->size.load(std::memory_order_relaxed))
            fp->size.store(tell, std::memory_order_relaxed);
        return FR_OK;
    }

    FRESULT f_lseek(FIL* fp, FSIZE_t ofs)
    {
        if(!fp || !fp->in_use.load(std::memory_order_relaxed))
            return FR_INVALID_OBJECT;
        if(::lseek(fp->fd, static_cast<off_t>(ofs), SEEK_SET) < 0)
            return FR_DISK_ERR;
        fp->tell.store(ofs, std::memory_order_relaxed);
        return FR_OK;
    }

    FRESULT f_sync(FIL* fp)
    {
        if(!fp || !fp->in_use.load(std::memory_order_relaxed))
            return FR_INVALID_OBJECT;
        return ::fsync(fp->fd) == 0 ? FR_OK : FR_DISK_ERR;
    }

    FRESULT f_truncate(FIL* fp)
    {
        if(!fp || !fp->in_use.load(std::memory_order_relaxed))
            return FR_INVALID_OBJECT;
        const int64_t tell = fp->tell.load(std::memory_order_relaxed);
        if(::ftruncate(fp->fd, static_cast<off_t>(tell)) != 0)
            return FR_DISK_ERR;
        fp->size.store(tell, std::memory_order_relaxed);
        return FR_OK;
    }

    FRESULT f_stat(const char* path, FILINFO* fno)
    {
        struct stat st;
        if(::stat(path, &st) != 0)
            return errno_to_fresult(errno);
        if(fno)
        {
            fno->fsize   = static_cast<FSIZE_t>(st.st_size);
            fno->fattrib = 0;
        }
        return FR_OK;
    }

    FRESULT f_unlink(const char* path)
    {
        if(::unlink(path) != 0)
            return errno_to_fresult(errno);
        return FR_OK;
    }

    FSIZE_t f_size(FIL* fp)
    {
        return fp ? static_cast<FSIZE_t>(fp->size.load(std::memory_order_relaxed)) : 0;
    }

    FSIZE_t f_tell(FIL* fp)
    {
        return fp ? static_cast<FSIZE_t>(fp->tell.load(std::memory_order_relaxed)) : 0;
    }

    int f_eof(FIL* fp)
    {
        if(!fp)
            return 1;
        return fp->tell.load(std::memory_order_relaxed) >= fp->size.load(std::memory_order_relaxed);
    }

} // extern "C"
