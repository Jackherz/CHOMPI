/**
 * engine/chompi_port.h — the only *new* header the vendored engine needs.
 *
 * CHOMPI names its samples with bare relative paths ("cubbi_a1.wav") because
 * the firmware always runs with the SD card root as the working directory.
 * The Move module must not chdir (it shares a process with stock Move), so
 * every filename the engine builds is routed through this header, which
 * prefixes a directory chosen at runtime:
 *
 *     set_param("sample_dir", "/data/UserData/.../chompi-samples")
 *
 * It also owns the SAMPLE MAP: a slot can be pointed at any WAV on the device
 * (`set_param("sample_file", <abs path>)`), which replaces the convention name
 * for that mode/bank/slot. The engine asks for a name and never cares which
 * of the two it gets.
 *
 * PORT PATCHES P4 (sample directory + naming), P9 (overrides), P10 (the erase
 * guard) — see VENDORED.md.
 */
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace chompi
{
/** Longest sample directory we accept. The engine's filename buffers are 256
 *  bytes, so the directory is clamped to leave room for "/cubbi_a4_double.wav"
 *  — a directory that did not fit would otherwise truncate the sample name and
 *  make every slot look empty. */
static constexpr size_t kSampleDirMax = 200;

/** Longest absolute sample path we store. Matches the engine's buffers. */
static constexpr size_t kSlotPathMax = 256;

/* --- the sample map -------------------------------------------------------
 * Keyed by (mode, bank, slot): 2 modes x 5 banks x 15 slots. Slot 15 is the
 * engine's recording/RAM slot and never holds a file, so it takes no entry;
 * the table keeps its row for a uniform index. */
static constexpr int kSlotModes = 2;
static constexpr int kSlotBanks = 5;
static constexpr int kSlotCount = 15;

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

/** The path the engine should use for one slot: the user's assigned sample if
 *  there is one, otherwise the convention name
 *  "<sample_dir>/<mode>_<bank><slot>[_double].wav".
 *
 *  `mode` is 0 (JAMMI) or 1 (CUBBI), `bank` 0..4, `slot` 1..15 (1-based, as the
 *  engine and KeyToSlot use it). A slot with an assigned sample resolves the
 *  same file for `dbl` — a user's WAV has no prepared double-speed companion,
 *  which is the honest answer: it plays, at the cost of the extra artefact the
 *  doubled copy exists to avoid. */
void SlotSamplePath(int mode, int bank, int slot, bool dbl, char* dst, size_t dst_len);

/** The convention name alone, ignoring any assignment:
 *  "<sample_dir>/<mode>_<bank><slot>[_double].wav". The browser hands back the
 *  path it was given, so an assignment equal to this one is not an override —
 *  it is what the slot already plays. */
void SlotConventionPath(int mode, int bank, int slot, bool dbl, char* dst, size_t dst_len);

/** Assigns the sample for one slot (absolute path), or clears it with an empty
 *  path. Write side of the map: called from set_param, i.e. the host's
 *  management thread — never the render callback. Slots outside 1..14 are
 *  refused (slot 15 is the recording buffer). */
void SetSlotOverride(int mode, int bank, int slot, const char* abs_path);
void ClearSlotOverride(int mode, int bank, int slot);
bool SlotHasOverride(int mode, int bank, int slot);

/** True when `path` lives inside the sample directory — i.e. it is a file this
 *  module owns and may unlink. A user-assigned sample from the device's library
 *  is NOT ours: CHOMPI's erase path would otherwise delete it. (PORT PATCH P10) */
bool IsManagedPath(const char* path);

/** Map persistence. Both call plain stdio and must NOT be called from the
 *  audio callback — the plugin runs them on its worker/loader threads. */
bool WriteSlotOverrideMap(const char* path); /* returns false if it could not be written */
int  ReadSlotOverrideMap(const char* path);  /* returns entries loaded, -1 if absent */

} // namespace chompi
