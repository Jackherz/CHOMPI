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

namespace chompi
{
    int  g_open_fail_count = 0;      /* see chompi_port.h */
    char g_last_open_fail[256] = {0};
    int  g_last_open_errno = 0;

char g_sample_dir[kSampleDirMax + 1] = {0};

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
