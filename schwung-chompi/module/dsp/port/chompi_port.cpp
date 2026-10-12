/**
 * port/chompi_port.cpp — the globals the vendored engine needs, plus the
 * sample map (PORT PATCHES P4/P9/P10).
 *
 * Threading: the map is WRITTEN from set_param (the SPI audio callback) and
 * READ from GetFileNameForSlot (also the callback), so ordinary use never
 * crosses threads. The one exception is persistence: the plugin's I/O worker
 * serialises the table to disk, and an assignment can land while it copies.
 * That copy is guarded by a seqlock — the writer bumps a counter before and
 * after, the reader retries if it changed, so the audio thread never blocks
 * and the worker never persists a half-written path.
 */
#include "chompi_port.h"

#include <atomic>
#include <cerrno>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace chompi
{
    int  g_open_fail_count = 0;      /* see chompi_port.h */
    char g_last_open_fail[256] = {0};
    int  g_last_open_errno = 0;

char g_sample_dir[kSampleDirMax + 1] = {0};

/* The portal defaults mirror the device: breakbeat's layout, one directory per
 * module under the user partition. Both are overridable at runtime for tests
 * (see set_param "portal_dir" / "user_library_dir") and both live in this file
 * only so the plugin does not have to know the layout. */
static const char* const kPortalDirDefault   = "/data/UserData/chompi-samples";
static const char* const kUserLibDirDefault  = "/data/UserData/UserLibrary/Samples";
static const char* const kBuiltInLinkName    = "Built-in";
static const char* const kUserLibLinkName    = "User Library";

char g_portal_dir[kSampleDirMax + 1] = {0};
char g_user_lib_dir[kSampleDirMax + 1] = {0};

namespace
{
/* Whether each portal link resolves to a directory. Cached by EnsureSamplePortal
 * so PresentSamplePath() (called on every get_param from the UI) does no I/O. */
bool g_builtin_link_ok  = false;
bool g_userlib_link_ok  = false;

inline bool DirExists(const char* path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool PathExists(const char* path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

/** True when `path` is `dir` or a child of it (never a sibling sharing a
 *  prefix: "/a/samples-old/x" is not inside "/a/samples"). */
inline bool InDir(const char* path, const char* dir)
{
    if(!path || !dir || !path[0] || !dir[0])
        return false;
    const size_t n = strlen(dir);
    if(strncmp(path, dir, n) != 0)
        return false;
    return path[n] == '\0' || path[n] == '/';
}

/** "<a>/<b>" into dst, never overrunning. */
inline void Join(char* dst, size_t dst_len, const char* a, const char* b)
{
    if(dst_len == 0)
        return;
    snprintf(dst, dst_len, b && b[0] ? "%s/%s" : "%s", a, b ? b : "");
}

/** Points `link` at `target`, replacing whatever was there. A pre-existing
 *  symlink is unlinked first so a moved sample directory re-points instead of
 *  failing with EEXIST; anything else (a real file, a real directory) is left
 *  alone — that is the user's, not ours.
 *
 *  The target is made absolute first: a relative one is resolved against the
 *  LINK's directory, not the process's, so "./portal/Built-in -> ./samples"
 *  would point at "portal/samples" and dangle. A sample directory that does not
 *  resolve is skipped — the portal is then simply not offered. */
bool ForceSymlink(const char* link, const char* target)
{
    char abs_target[kPortalPathMax];
    if(realpath(target, abs_target) == nullptr)
        return false;

    struct stat st;
    if(lstat(link, &st) == 0)
    {
        if(!S_ISLNK(st.st_mode))
            return false; /* not ours to replace */
        (void)unlink(link);
    }
    return symlink(abs_target, link) == 0;
}
} // namespace

namespace
{
char             g_map[kSlotModes][kSlotBanks][kSlotCount][kSlotPathMax];
std::atomic<uint32_t> g_map_seq{0};

inline bool Valid(int mode, int bank, int slot)
{
    return mode >= 0 && mode < kSlotModes && bank >= 0 && bank < kSlotBanks
           && slot >= 1 && slot <= kSlotCount;
}

inline void MapWriteBegin() { g_map_seq.fetch_add(1, std::memory_order_release); }
inline void MapWriteEnd() { g_map_seq.fetch_add(1, std::memory_order_release); }

/** Copies one path in, or clears it. Caller brackets with Begin/End. */
inline void Put(int mode, int bank, int slot, const char* abs_path)
{
    char* dst = g_map[mode][bank][slot - 1];
    if(abs_path == nullptr)
        abs_path = "";
    const size_t n = strnlen(abs_path, kSlotPathMax - 1);
    memcpy(dst, abs_path, n);
    dst[n] = '\0';
}
} // namespace

/* --- per-slot values (PORT PATCH P19) -------------------------------------
 *
 * Same seqlock discipline as the sample map: the audio thread reads these on
 * every key press (the CUBBI path loads the pressed pad's values), the host's
 * thread writes them when a knob moves. A torn read would put a wrong attack
 * time on one note, so writers bump g_values_seq and readers of the whole slot
 * (LoadSlotValues in the engine) retry when the sequence moved under them.
 */
float g_values[kSlotModes][kSlotBanks][kSlotCount][kSlotValueCount];
bool  g_values_touched[kSlotModes][kSlotBanks][kSlotCount];
std::atomic<uint32_t> g_values_seq{0};
uint32_t g_values_writes = 0;

inline void ValuesWriteBegin() { g_values_seq.fetch_add(1, std::memory_order_release); }
inline void ValuesWriteEnd()   { g_values_seq.fetch_add(1, std::memory_order_release); }

/* The hardware's defaults for a slot nobody has touched (ui.h enc_defaults,
 * read through the same mapping OpenCubbiSlot uses). */
inline float DefaultValue(int field)
{
    switch(field)
    {
        case kSlotPitch:   return .83f;   /* 1x: .83 on the pitch encoder's own curve */
        case kSlotStart:   return 0.f;
        case kSlotEnd:     return 1.f;
        case kSlotAttack:  return 0.f;
        case kSlotDecay:   return 0.f;
        case kSlotLoop:    return 1.f;
        case kSlotSustain: return 1.f;
        case kSlotGain:    return .704f;  /* unity: SetGain maps .704 -> 1.0 */
        case kSlotPan:     return .5f;
        default:           return 0.f;
    }
}

float GetSlotValue(int mode, int bank, int slot, int field)
{
    if(!Valid(mode, bank, slot) || field < 0 || field >= kSlotValueCount)
        return DefaultValue(field);
    if(!g_values_touched[mode][bank][slot - 1])
        return DefaultValue(field);
    return g_values[mode][bank][slot - 1][field];
}

void SetSlotValue(int mode, int bank, int slot, int field, float v)
{
    if(!Valid(mode, bank, slot) || field < 0 || field >= kSlotValueCount)
        return;

    ValuesWriteBegin();
    float* row = g_values[mode][bank][slot - 1];
    if(!g_values_touched[mode][bank][slot - 1])
    {
        /* First touch of this slot: start the row from the defaults, so the
         * fields the user is NOT turning keep the hardware's values rather than
         * the zero the array happened to hold. Getting this wrong is not
         * subtle: the first knob on a slot used to set that slot's end point to
         * 0, i.e. every subsequent key press played an empty region of the file
         * -- silence, from a knob that has nothing to do with the end point. */
        for(int f = 0; f < kSlotValueCount; f++)
            row[f] = DefaultValue(f);
        g_values_touched[mode][bank][slot - 1] = true;
    }
    row[field] = v;
    ValuesWriteEnd();
    g_values_writes++;
}

unsigned SlotValuesWriteCount()
{
    return g_values_writes;
}

bool SlotValuesTouched(int mode, int bank, int slot)
{
    if(!Valid(mode, bank, slot))
        return false;
    return g_values_touched[mode][bank][slot - 1];
}

bool WriteSlotValues(const char* path)
{
    if(path == nullptr || path[0] == '\0')
        return false;

    static float snapshot[kSlotModes][kSlotBanks][kSlotCount][kSlotValueCount];
    static bool  touched[kSlotModes][kSlotBanks][kSlotCount];
    for(int attempt = 0; attempt < 64; attempt++)
    {
        const uint32_t before = g_values_seq.load(std::memory_order_acquire);
        if(before & 1u)
            continue;
        memcpy(snapshot, g_values, sizeof(g_values));
        memcpy(touched, g_values_touched, sizeof(g_values_touched));
        if(g_values_seq.load(std::memory_order_acquire) == before)
            break;
    }

    char tmp[kSlotPathMax + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if(f == nullptr)
        return false;

    int written = 0;
    for(int m = 0; m < kSlotModes; m++)
        for(int b = 0; b < kSlotBanks; b++)
            for(int s = 0; s < kSlotCount; s++)
            {
                if(!touched[m][b][s])
                    continue;
                const float* v = snapshot[m][b][s];
                if(fprintf(f, "%d %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n",
                           m, b, s + 1, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]) > 0)
                    written++;
            }
    const bool ok = fclose(f) == 0;
    if(!ok)
    {
        remove(tmp);
        return false;
    }
    (void)written;
    return rename(tmp, path) == 0;
}

int ReadSlotValues(const char* path)
{
    if(path == nullptr || path[0] == '\0')
        return -1;
    FILE* f = fopen(path, "r");
    if(f == nullptr)
        return -1;

    /* The file is the whole truth: clear the table first so a slot the user
     * reset while the module was unloaded does not come back. */
    ValuesWriteBegin();
    memset(g_values, 0, sizeof(g_values));
    memset(g_values_touched, 0, sizeof(g_values_touched));
    ValuesWriteEnd();

    char line[256];
    int  loaded = 0;
    while(fgets(line, sizeof line, f) != nullptr)
    {
        if(line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;
        int   m = -1, b = -1, s = -1;
        float v[kSlotValueCount];
        const int n = sscanf(line, "%d %d %d %f %f %f %f %f %f %f %f %f",
                             &m, &b, &s, &v[0], &v[1], &v[2], &v[3], &v[4],
                             &v[5], &v[6], &v[7], &v[8]);
        if(n < 3 + 1)
            continue;
        /* Fields the file does not carry keep their default: an older file with
         * fewer columns still loads the ones it has. */
        for(int field = 0; field < kSlotValueCount; field++)
        {
            const float value = (3 + field < n) ? v[field] : DefaultValue(field);
            SetSlotValue(m, b, s, field, value);
        }
        loaded++;
    }
    fclose(f);
    return loaded;
}

void SetSlotOverride(int mode, int bank, int slot, const char* abs_path)
{
    if(!Valid(mode, bank, slot))
        return;
    MapWriteBegin();
    Put(mode, bank, slot, abs_path);
    MapWriteEnd();
}

void ClearSlotOverride(int mode, int bank, int slot)
{
    SetSlotOverride(mode, bank, slot, "");
}

bool SlotHasOverride(int mode, int bank, int slot)
{
    if(!Valid(mode, bank, slot))
        return false;
    return g_map[mode][bank][slot - 1][0] != '\0';
}

void SlotSamplePath(int mode, int bank, int slot, bool dbl, char* dst, size_t dst_len)
{
    if(dst == nullptr || dst_len == 0)
        return;

    if(Valid(mode, bank, slot) && g_map[mode][bank][slot - 1][0] != '\0')
    {
        const size_t n = strnlen(g_map[mode][bank][slot - 1], dst_len - 1);
        memcpy(dst, g_map[mode][bank][slot - 1], n);
        dst[n] = '\0';
        return;
    }

    SlotConventionPath(mode, bank, slot, dbl, dst, dst_len);
}

void SlotConventionPath(int mode, int bank, int slot, bool dbl, char* dst, size_t dst_len)
{
    if(dst == nullptr || dst_len == 0)
        return;

    /* The convention name, exactly as the engine used to build it:
     * "<mode>_<bankchar><slot>[_double].wav". */
    char bare[64];
    snprintf(bare, sizeof bare, "%s_%c%1d%s.wav",
             (mode == 1) ? "cubbi" : "jammi",
             (char)('a' + ((bank >= 0 && bank < kSlotBanks) ? bank : 0)),
             slot,
             dbl ? "_double" : "");
    SamplePath(dst, dst_len, bare);
}

bool EnsureSamplePortal()
{
    /* Default to the device layout on first use, so create_instance() does not
     * have to pass anything. */
    if(g_portal_dir[0] == '\0')
        SetPortalDir(kPortalDirDefault);
    if(g_user_lib_dir[0] == '\0')
        SetUserLibraryDir(kUserLibDirDefault);

    g_builtin_link_ok = false;
    g_userlib_link_ok = false;

    char link[kPortalPathMax];

    /* A portal whose parent does not exist is not an error to shout about: the
     * same binary runs on a host for the offline probes, where /data is not
     * there and absolute paths work perfectly well. */
    struct stat st;
    bool ok = (mkdir(g_portal_dir, 0755) == 0) || (stat(g_portal_dir, &st) == 0 && S_ISDIR(st.st_mode));
    if(!ok)
        return false;

    if(g_sample_dir[0] != '\0')
    {
        Join(link, sizeof link, g_portal_dir, kBuiltInLinkName);
        if(ForceSymlink(link, g_sample_dir) && PathExists(link))
            g_builtin_link_ok = true;
    }

    if(DirExists(g_user_lib_dir))
    {
        Join(link, sizeof link, g_portal_dir, kUserLibLinkName);
        if(ForceSymlink(link, g_user_lib_dir) && PathExists(link))
            g_userlib_link_ok = true;
    }

    return g_builtin_link_ok || g_userlib_link_ok;
}

void PresentSamplePath(const char* path, char* dst, size_t dst_len)
{
    if(dst == nullptr || dst_len == 0)
        return;
    if(path == nullptr || path[0] == '\0')
    {
        dst[0] = '\0';
        return;
    }

    char        probe[kPortalPathMax];
    const char* rest = nullptr;
    const char* sub  = nullptr;

    if(g_builtin_link_ok && InDir(path, g_sample_dir) && strcmp(path, g_sample_dir) != 0)
    {
        rest = path + strlen(g_sample_dir) + 1;
        sub  = kBuiltInLinkName;
    }
    else if(g_userlib_link_ok && InDir(path, g_user_lib_dir) && strcmp(path, g_user_lib_dir) != 0)
    {
        rest = path + strlen(g_user_lib_dir) + 1;
        sub  = kUserLibLinkName;
    }

    if(sub != nullptr)
    {
        Join(probe, sizeof probe, g_portal_dir, sub);
        snprintf(dst, dst_len, "%s/%s", probe, rest);
        return;
    }

    snprintf(dst, dst_len, "%s", path);
}

void CanonicalSamplePath(const char* path, char* dst, size_t dst_len)
{
    if(dst == nullptr || dst_len == 0)
        return;
    if(path == nullptr || path[0] == '\0')
    {
        dst[0] = '\0';
        return;
    }

    /* Only a path that names a portal entry is rewritten. Everything else is
     * passed through EXACTLY as given — including a relative one like
     * "./samples/cubbi_a1.wav", which the engine resolves against its own
     * (possibly also relative) sample directory and which is what the host
     * writes back when its browser is cancelled. Re-basing every relative path
     * on the portal, which is the obvious-looking simplification, silently
     * rewrites that value into a path that does not exist. */
    char        link[kPortalPathMax];
    const char* rest = nullptr;
    const char* target = nullptr;

    Join(link, sizeof link, g_portal_dir[0] ? g_portal_dir : kPortalDirDefault, kBuiltInLinkName);
    if(InDir(path, link))
    {
        rest   = path + strlen(link);
        target = g_sample_dir;
    }
    else
    {
        Join(link, sizeof link, g_portal_dir[0] ? g_portal_dir : kPortalDirDefault, kUserLibLinkName);
        if(InDir(path, link))
        {
            rest   = path + strlen(link);
            target = g_user_lib_dir;
        }
    }

    if(rest != nullptr && target[0] != '\0')
    {
        snprintf(dst, dst_len, "%s%s", target, rest);
        return;
    }

    snprintf(dst, dst_len, "%s", path);
}

int DescribePortal(char* buf, size_t buf_len)
{
    if(buf == nullptr || buf_len == 0)
        return 0;

    char link[kPortalPathMax];
    Join(link, sizeof link, g_portal_dir[0] ? g_portal_dir : kPortalDirDefault, kBuiltInLinkName);
    const bool builtin = PathExists(link);
    Join(link, sizeof link, g_portal_dir[0] ? g_portal_dir : kPortalDirDefault, kUserLibLinkName);
    const bool userlib = PathExists(link);

    /* What the browser will do with this: the root, whether each link resolves,
     * and what the current sample directory is (a stale `Built-in` shows up
     * here as a mismatch). */
    const int n = snprintf(buf, buf_len,
                           "root=%s built-in=%s%s user-library=%s%s samples=%s",
                           g_portal_dir[0] ? g_portal_dir : kPortalDirDefault,
                           builtin ? "ok" : "missing", builtin ? "" : "(recreate: any set_param)",
                           userlib ? "ok" : "missing", userlib ? "" : "(no such directory)",
                           g_sample_dir[0] ? g_sample_dir : "(cwd)");
    return (n > 0 && n < (int)buf_len) ? n : 0;
}

bool IsManagedPath(const char* path)
{
    if(path == nullptr || path[0] == '\0')
        return false;
    if(g_sample_dir[0] == '\0')
        return false; /* no directory configured: we own nothing */
    const size_t dir_len = strlen(g_sample_dir);
    if(strncmp(path, g_sample_dir, dir_len) != 0)
        return false;
    /* Must be the directory itself or a child of it, never a sibling that
     * merely shares the prefix ("…/samples-old/x.wav"). */
    return path[dir_len] == '\0' || path[dir_len] == '/';
}

bool WriteSlotOverrideMap(const char* path)
{
    if(path == nullptr || path[0] == '\0')
        return false;

    /* Snapshot under the seqlock: retry while a callback write is in flight. */
    static char snapshot[kSlotModes][kSlotBanks][kSlotCount][kSlotPathMax];
    for(int attempt = 0; attempt < 64; attempt++)
    {
        const uint32_t before = g_map_seq.load(std::memory_order_acquire);
        if(before & 1u)
            continue;
        memcpy(snapshot, g_map, sizeof(g_map));
        if(g_map_seq.load(std::memory_order_acquire) == before)
            break;
    }

    char tmp[kSlotPathMax + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if(f == nullptr)
        return false;

    int written = 0;
    for(int m = 0; m < kSlotModes; m++)
        for(int b = 0; b < kSlotBanks; b++)
            for(int s = 0; s < kSlotCount; s++)
            {
                const char* p = snapshot[m][b][s];
                if(p[0] == '\0')
                    continue;
                /* "<mode> <bank> <slot-1based> <path>" — one line each, the
                 * path last so it may contain spaces. */
                if(fprintf(f, "%d %d %d %s\n", m, b, s + 1, p) > 0)
                    written++;
            }
    const bool ok = fclose(f) == 0;
    if(!ok || written == 0)
    {
        remove(tmp);
        /* An empty map is written as an empty file rather than removed: the
         * caller distinguishes "no assignments" from "no file yet". */
        if(ok && written == 0)
        {
            FILE* e = fopen(path, "w");
            if(e != nullptr)
                fclose(e);
        }
        return ok;
    }
    if(rename(tmp, path) != 0)
    {
        remove(tmp);
        return false;
    }
    return true;
}

int ReadSlotOverrideMap(const char* path)
{
    if(path == nullptr || path[0] == '\0')
        return -1;
    FILE* f = fopen(path, "r");
    if(f == nullptr)
        return -1;

    /* Clear first: the file is the whole truth, so a slot removed by hand
     * while the module was unloaded does not survive as a stale entry. */
    MapWriteBegin();
    memset(g_map, 0, sizeof(g_map));
    MapWriteEnd();

    char line[kSlotPathMax + 32];
    int  loaded = 0;
    while(fgets(line, sizeof line, f) != nullptr)
    {
        int mode = 0, bank = 0, slot = 0;
        if(sscanf(line, "%d %d %d", &mode, &bank, &slot) != 3)
            continue;
        /* The path is whatever follows the third field, minus the newline. */
        const char* p = line;
        for(int field = 0; field < 3 && *p; field++)
        {
            while(*p == ' ')
                p++;
            while(*p && *p != ' ')
                p++;
        }
        while(*p == ' ')
            p++;
        size_t n = strcspn(p, "\r\n");
        if(n == 0)
            continue;
        char tmp[kSlotPathMax];
        if(n >= sizeof tmp)
            n = sizeof tmp - 1;
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        if(Valid(mode, bank, slot))
        {
            SetSlotOverride(mode, bank, slot, tmp);
            loaded++;
        }
    }
    fclose(f);
    return loaded;
}

} // namespace chompi
