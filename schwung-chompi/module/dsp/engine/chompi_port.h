/**
 * engine/chompi_port.h — the only *new* header the vendored engine needs.
 *
 * CHOMPI names its samples with bare relative paths ("cubbi_a1.wav") because
 * the firmware always runs with the SD card root as the working directory.
 * The Move module must not chdir (it shares a process with stock Move), so
 * every filename the engine builds is routed through chompi::SamplePath(),
 * which prefixes a directory chosen at runtime:
 *
 *     set_param("sample_dir", "/data/UserData/.../chompi-samples")
 *
 * PORT PATCH P4 (see VENDORED.md) is the three call sites that use this.
 */
#pragma once

#include <cstddef>
#include <cstring>

namespace chompi
{
/** Longest sample directory we accept. The engine's filename buffers are 256
 *  bytes, so the directory is clamped to leave room for "/cubbi_a4_double.wav"
 *  — a directory that did not fit would otherwise truncate the sample name and
 *  make every slot look empty. */
static constexpr size_t kSampleDirMax = 200;

/** Directory that holds jammi_*.wav / cubbi_*.wav / looper.wav / temp_rec.wav.
 *  Written by the plugin on the loader thread; read by the I/O worker. */
extern char g_sample_dir[kSampleDirMax + 1];

/** Sets the directory, clamped to kSampleDirMax bytes. Empty = "no prefix",
 *  which is the firmware behaviour (paths resolve against the cwd). */
inline void SetSampleDir(const char* dir)
{
    if(dir == nullptr)
        dir = "";
    /* strnlen+memcpy rather than snprintf: the clamp is deliberate, and this
     * spelling states it instead of tripping -Wformat-truncation. */
    const size_t n = strnlen(dir, kSampleDirMax);
    memcpy(g_sample_dir, dir, n);
    g_sample_dir[n] = '\0';
}

/** Builds "<sample_dir>/<name>" into dst (dst_len bytes; never overflows). */
inline void SamplePath(char* dst, size_t dst_len, const char* name)
{
    if(dst_len == 0)
        return;
    size_t off = 0;
    if(chompi::g_sample_dir[0] != '\0')
    {
        off = strnlen(chompi::g_sample_dir, dst_len - 1);
        memcpy(dst, chompi::g_sample_dir, off);
        if(off < dst_len - 1)
            dst[off++] = '/';
    }
    const size_t n = strnlen(name, dst_len - 1 - off);
    memcpy(dst + off, name, n);
    dst[off + n] = '\0';
}

} // namespace chompi
