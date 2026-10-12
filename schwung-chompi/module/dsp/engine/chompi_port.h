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
    /** The last f_open() the compat layer refused, and how many it has. A
     *  failed open is not cosmetic: the engine's reader counts a pending open
     *  before queueing it and only clears the count once the open lands, so a
     *  failure wedges that voice's streaming for good (`debug_voice`). */
    extern int  g_open_fail_count;
    extern char g_last_open_fail[256];
    extern int  g_last_open_errno;

    /** PORT PATCH P21: the sample translator (port/sample_view.h). `g_sample_view_count`
     *  counts the files this session has had to convert, and `g_last_sample_view`
     *  says what the most recent one was -- "24-bit stereo -> 16-bit stereo"
     *  or "format=17 bits=4 ch=1 -> silence (not decodable)". A user sample
     *  that needed translating is otherwise invisible: it just sounds right.
     *  Surfaced by `debug_wav` and by `debug_voice`. */
    extern int  g_sample_view_count;
    extern char g_last_sample_view[160];

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
 *  same file for `dbl`, because a user's WAV has no prepared double-speed
 *  companion: it plays, pitched over the whole octave, at the cost of the
 *  aliasing the doubled copy exists to avoid. `SlotHasDoubleSpeedCopy()` below
 *  is what keeps the reader from claiming otherwise. */
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

/** Does this slot's sample have a prepared 2x-speed companion to swap to?
 *
 *  PORT PATCH P22. The card's convention names come in pairs: `<name>.wav`
 *  plus `<name>_double.wav`, the same audio at twice the SPEED — half the
 *  frames, the content an octave up (measured 140 Hz in `cubbi_a1.wav` against
 *  280 Hz in its double) — so that once varispeed * global pitch passes 1.5x
 *  the reader has samples to read that the ordinary copy, playing that fast,
 *  would be skipping. Every convention name on the card has a companion (84
 *  pairs, none missing). A file the USER assigned to a slot has none, and
 *  `SlotSamplePath()` resolves both names to the one file, so a reader that
 *  claimed double speed played that file an octave low (measured 321 Hz where
 *  642 was due). Hence: convention name = yes, assignment = no. */
bool SlotHasDoubleSpeedCopy(int mode, int bank, int slot);

/** True when `path` lives inside the sample directory — i.e. it is a file this
 *  module owns and may unlink. A user-assigned sample from the device's library
 *  is NOT ours: CHOMPI's erase path would otherwise delete it. (PORT PATCH P10) */
bool IsManagedPath(const char* path);

/* --- per-slot values (PORT PATCH P19) -------------------------------------
 *
 * The hardware keeps nine values per (mode, bank, slot) in its preset store and
 * re-reads the ones belonging to a slot every time one of that slot's pads is
 * pressed (NormalPage::OpenCubbiSlot -> DSPEngine::OpenCubbiSlot). The port
 * needs the same table. Without it the Tape page's knobs were ONE global set of
 * values shared by every slot and bank, so moving the Setup page's bank or slot
 * changed nothing you could see or hear -- which is exactly what was reported
 * from the device: "bank/slot unclear, changing them changes nothing".
 *
 * Field order follows the hardware's preset indexes (PresetManager::SetValue).
 * Values are plain floats; `loop` and `sustain` are 1.0/0.0. */
enum SlotValue
{
    kSlotPitch = 0,
    kSlotStart,
    kSlotEnd,
    kSlotAttack,
    kSlotDecay,
    kSlotLoop,
    kSlotSustain,
    kSlotGain,
    kSlotPan,
    kSlotValueCount
};

/** The value in effect for a slot: the stored one if the slot was ever
 *  touched, otherwise the hardware's default for an untouched slot
 *  (ui.h enc_defaults: pitch .83 == 1x, start 0, end 1, attack 0, decay 0,
 *  loop on, sustain on, gain .704 == unity, pan centre). */
float GetSlotValue(int mode, int bank, int slot, int field);

/** Writes one value. Called from set_param (the host's management thread). */
void SetSlotValue(int mode, int bank, int slot, int field, float v);

/** True when any value of that slot differs from the defaults -- i.e. the slot
 *  is worth writing to the values file. */
bool SlotValuesTouched(int mode, int bank, int slot);

/** Everything the user has changed, as text: one line per touched slot,
 *  "<mode> <bank> <slot> <9 floats>". Both return false/-1 on I/O failure; a
 *  missing file is not an error (nothing changed yet). */
bool WriteSlotValues(const char* path);
int  ReadSlotValues(const char* path);

/** How many values have been written since the module loaded. The I/O worker
 *  watches this to know when the store needs saving (it is bumped by every
 *  knob that edits a slot, from any thread). */
unsigned SlotValuesWriteCount();

/* --- the sample portal (PORT PATCH P18) -----------------------------------
 *
 * Where the file browser starts, and what a sample path looks like when the UI
 * sees it.
 *
 * The chain host clamps a filepath browser to the parameter's `root` and starts
 * it on the parameter's current value (see Schwung's filepath_browser.mjs:
 * current value → start_path → root). Pointing that root at /data lets the user
 * wander the whole device, and pointing it at the user's library alone hides
 * the samples that ship with the module. So the module keeps a portal — a
 * directory whose entries are symlinks:
 *
 *     /data/UserData/chompi-samples/
 *     ├── Built-in      -> <module dir>/samples      (the tape-2.0 card set)
 *     └── User Library  -> /data/UserData/UserLibrary/Samples
 *
 * and the filepath parameters are rooted there. One directory to open, both
 * sources side by side, nothing outside it reachable. This is mestela's
 * schwung-breakbeat layout (ensure_portal_exists in breakbeat.c), adopted here
 * because it solves the same three problems.
 *
 * Paths are then presented THROUGH the portal and stored BESIDE it: the engine
 * and the slot map keep canonical absolute paths (a user sample lives at its
 * real location in the library), while the value the UI reads and writes is the
 * portal path. CanonicalSamplePath() maps what the browser hands back onto the
 * real file, so an assignment that names a shipped sample through the portal
 * still compares equal to the slot's convention path and clears the override
 * instead of pinning a symlink route into the map.
 *
 * All of this is management-thread work (create + set_param): it touches the
 * filesystem exactly the way the sample map does, and never runs on the audio
 * callback. */

/** Longest portal path we build: "<portal>/<link>/<sample name>". */
static constexpr size_t kPortalPathMax = kSampleDirMax + 64;

/** Browser root. Set by the plugin at create (and by set_param "portal_dir"). */
extern char g_portal_dir[kSampleDirMax + 1];

/** The user's sample library. Set by the plugin at create (and by set_param
 *  "user_library_dir"); only exposed when the directory actually exists. */
extern char g_user_lib_dir[kSampleDirMax + 1];

inline void SetPortalDir(const char* dir)
{
    const char* d = (dir == nullptr) ? "" : dir;
    const size_t n = strnlen(d, kSampleDirMax);
    memcpy(g_portal_dir, d, n);
    g_portal_dir[n] = '\0';
}

inline void SetUserLibraryDir(const char* dir)
{
    const char* d = (dir == nullptr) ? "" : dir;
    const size_t n = strnlen(d, kSampleDirMax);
    memcpy(g_user_lib_dir, d, n);
    g_user_lib_dir[n] = '\0';
}

/** Creates the portal directory and its two symlinks if it can, re-pointing
 *  `Built-in` at the current sample directory each time (the sample directory
 *  can be moved with set_param "sample_dir"). Idempotent, cheap, and quiet when
 *  the device cannot support it — the browser then simply falls back to
 *  absolute paths, which still work. Returns true when the portal is usable. */
bool EnsureSamplePortal();

/** Canonical path → what the UI should see. A sample inside the sample
 *  directory is presented as "<portal>/Built-in/<name>", one inside the user
 *  library as "<portal>/User Library/<name>", and anything else unchanged (if
 *  the matching link does not exist, the canonical path is returned: a path
 *  that cannot be opened would break the waveform the host draws from it). */
void PresentSamplePath(const char* path, char* dst, size_t dst_len);

/** What the browser hands back → the path to store and open. The inverse of
 *  PresentSamplePath(), plus a relative path is resolved against the portal. */
void CanonicalSamplePath(const char* path, char* dst, size_t dst_len);

/** One line for the diagnostics: the portal, whether each link resolves, and
 *  what the browser would open on. */
int DescribePortal(char* buf, size_t buf_len);

/** Map persistence. Both call plain stdio and must NOT be called from the
 *  audio callback — the plugin runs them on its worker/loader threads. */
bool WriteSlotOverrideMap(const char* path); /* returns false if it could not be written */
int  ReadSlotOverrideMap(const char* path);  /* returns entries loaded, -1 if absent */

} // namespace chompi
