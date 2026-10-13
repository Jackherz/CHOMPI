/*
 * tools/hierarchy_probe.cpp — parse the module's pages the way the host does.
 *
 * The report this answers: after a deploy the Move showed a single preset page
 * reading "no presets" and none of the module's pages. Nothing was wrong with
 * the DSP, the params or the engine -- the hierarchy string had two stray
 * quotes in one level's param list, the host's parser rejected the whole
 * document, and its fallback is exactly that empty preset page. Every other
 * probe in this directory asks for params BY KEY, so all of them passed on the
 * device while the module was unusable by hand. That is the gap this fills.
 *
 * What it pins:
 *   - ui_hierarchy and chain_params must both be valid JSON (syntax);
 *   - every key named in a page must be a param chain_params serves -- a typo
 *     is otherwise a knob that silently never appears;
 *   - every `level` a page links to must exist, and the root must be there;
 *   - a level's knobs must be keys of that level's own param list;
 *   - no param may be listed twice in one level (two knobs on one key).
 *
 * The module emits its levels in a fixed field order -- `label`, `params`,
 * `knobs` -- and two checks lean on it: a level's knobs are matched against the
 * param list just before them, and a level's name is read from the `":{"label":`
 * that opens it. If that order ever changes these checks have to change with
 * it; they fail loudly rather than passing quietly.
 *
 * `--selftest` runs the checks over two literals -- a good hierarchy and the
 * broken Looper list from that incident -- so the guard is shown to catch the
 * real thing rather than asserted to.
 *
 * Usage:  ./hierarchy_probe build/chompi_move.so [state_dir]
 *         ./hierarchy_probe --selftest
 */
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "host/plugin_api_v1.h"

static int g_fails = 0;

static void check(bool ok, const char* what, const char* detail = nullptr)
{
    if(!ok) g_fails++;
    if(detail && detail[0]) printf("%-4s %s  %s\n", ok ? "ok" : "FAIL", what, detail);
    else                    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
}

static void note(const char* fmt, ...)
{
    printf("       ");
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    printf("\n");
}

/* --- enough of a JSON reader to be one ----------------------------------
 * Two kilobytes of JSON, one question: does the host's parser accept this?
 * Strings are copied raw (the module writes no escapes inside the names it
 * serves) but escaped characters are still parsed, so a stray backslash is a
 * syntax error here exactly as it is in the host. */
struct JsonReader {
    const char* p;
    const char* end;
    void ws(){ while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool string()
    {
        if(p >= end || *p != '"') return false;
        for(p++; p < end; p++){
            if(*p == '\\'){
                if(p + 1 >= end) return false;
                p++;
                if(*p == 'u'){
                    for(int i = 0; i < 4; i++){ p++;
                        if(p >= end || !((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                                         (*p >= 'A' && *p <= 'F'))) return false; }
                } else if(strchr("\"\\/bfnrt", *p) == nullptr){
                    return false;
                }
                continue;
            }
            if(*p == '"'){ p++; return true; }
            if((unsigned char)*p < 0x20) return false;
        }
        return false;
    }
    bool number()
    {
        const char* s = p;
        if(p < end && *p == '-') p++;
        const char* digits = p;
        while(p < end && *p >= '0' && *p <= '9') p++;
        if(p == digits) return false;
        if(p < end && *p == '.'){ p++; const char* d = p;
            while(p < end && *p >= '0' && *p <= '9') p++;
            if(p == d) return false; }
        if(p < end && (*p == 'e' || *p == 'E')){ p++;
            if(p < end && (*p == '+' || *p == '-')) p++;
            const char* d = p;
            while(p < end && *p >= '0' && *p <= '9') p++;
            if(p == d) return false; }
        return p > s;
    }
    bool value(int depth)
    {
        if(depth > 24) return false;
        ws();
        if(p >= end) return false;
        if(*p == '{') return object(depth);
        if(*p == '[') return array(depth);
        if(*p == '"') return string();
        if(*p == '-' || (*p >= '0' && *p <= '9')) return number();
        if(end - p >= 4 && strncmp(p, "true", 4) == 0){ p += 4; return true; }
        if(end - p >= 5 && strncmp(p, "false", 5) == 0){ p += 5; return true; }
        if(end - p >= 4 && strncmp(p, "null", 4) == 0){ p += 4; return true; }
        return false;
    }
    bool object(int depth)
    {
        p++;
        ws();
        if(p < end && *p == '}'){ p++; return true; }
        for(;;){
            ws();
            if(!string()) return false;
            ws();
            if(p >= end || *p != ':') return false;
            p++;
            if(!value(depth + 1)) return false;
            ws();
            if(p >= end) return false;
            if(*p == ','){ p++; continue; }
            if(*p == '}'){ p++; return true; }
            return false;
        }
    }
    bool array(int depth)
    {
        p++;
        ws();
        if(p < end && *p == ']'){ p++; return true; }
        for(;;){
            if(!value(depth + 1)) return false;
            ws();
            if(p >= end) return false;
            if(*p == ','){ p++; continue; }
            if(*p == ']'){ p++; return true; }
            return false;
        }
    }
};

static bool IsJson(const char* buf, int len)
{
    if(len <= 0) return false;
    JsonReader r{buf, buf + len};
    r.ws();
    if(!r.value(0)) return false;
    r.ws();
    return r.p == r.end;
}

/* Where a JSON string that starts at `p` (which must be a quote) ends. */
static const char* SkipString(const char* p, const char* end)
{
    for(p++; p < end; p++){
        if(*p == '\\'){ p++; continue; }
        if(*p == '"') return p + 1;
    }
    return nullptr;
}

/* Copy the value of `"field":"VALUE"` at or after `from`. Whitespace around the
 * colon is allowed -- chain_params is written with spaces, ui_hierarchy without. */
static const char* StringValue(const char* from, const char* end, const char* field,
                               char* out, int cap)
{
    const char* p = strstr(from, field);
    if(!p || p >= end) return nullptr;
    p += strlen(field);
    while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if(p >= end || *p != ':') return nullptr;
    p++;
    while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if(p >= end || *p != '"') return nullptr;
    const char* q = p + 1;
    int n = 0;
    while(q < end && *q != '"'){ if(n < cap - 1) out[n++] = *q; q++; }
    if(q >= end) return nullptr;
    out[n] = 0;
    return q + 1;
}

struct NameSet {
    char v[128][40];
    int  n = 0;
    bool has(const char* s) const { for(int i = 0; i < n; i++) if(strcmp(v[i], s) == 0) return true; return false; }
    bool add(const char* s){ if(!has(s) && n < 128){ strncpy(v[n], s, 39); v[n][39] = 0; n++; return true; } return false; }
};

/* Every `"key":"..."` in the text, in order. */
static void CollectKeys(const char* from, const char* end, NameSet& out, int* count = nullptr)
{
    char name[40];
    const char* p = from;
    while(true){
        const char* next = StringValue(p, end, "\"key\"", name, sizeof name);
        if(!next) break;
        out.add(name);
        if(count) (*count)++;
        p = next;
    }
}

/* The strings inside the array that opens at `open` (which points at `[`). */
static const char* CollectArrayStrings(const char* open, const char* end, NameSet& out, int* count)
{
    const char* p = open + 1;
    for(;;){
        while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
        if(p >= end) return nullptr;
        if(*p == ']') return p + 1;
        if(*p != '"') return nullptr;
        char name[40];
        int n = 0;
        const char* q = p + 1;
        while(q < end && *q != '"'){ if(n < 39) name[n++] = *q; q++; }
        if(q >= end) return nullptr;
        name[n] = 0;
        out.add(name);
        if(count) (*count)++;
        p = q + 1;
    }
}

/* Walk the levels in emission order and check each one against itself. */
static void CheckLevels(const char* buf, const char* end, NameSet& defined,
                        int* params_total, int* knobs_total)
{
    const char* p = buf;
    while(true){
        const char* open = strstr(p, "\":{\"label\":");
        if(!open || open >= end) break;
        /* the level's name is the quoted token that ends here */
        const char* start = open;
        while(start > buf && start[-1] != '"') start--;
        int n = 0;
        char name[40];
        for(const char* s = start; s < open && n < 39; s++) name[n++] = *s;
        name[n] = 0;
        defined.add(name);

        const char* params = strstr(open, "\"params\":[");
        const char* knobs  = params ? strstr(params, "\"knobs\":[") : nullptr;
        if(!params || !knobs || knobs >= end){
            check(false, "a level lists its params and its knobs", name);
            p = open + 1;
            continue;
        }
        NameSet keys;
        int key_count = 0, dup = 0;
        {
            char key[40];
            const char* q = params;
            while(true){
                const char* next = StringValue(q, knobs, "\"key\"", key, sizeof key);
                if(!next) break;
                if(!keys.add(key)) dup++;
                key_count++;
                q = next;
            }
        }
        NameSet knob_names;
        int knob_count = 0;
        const char* open_bracket = strchr(knobs, '[');
        const char* after = open_bracket ? CollectArrayStrings(open_bracket, end, knob_names, &knob_count) : nullptr;

        bool every_knob_is_a_param = true;
        for(int i = 0; i < knob_names.n; i++)
            if(!keys.has(knob_names.v[i])) every_knob_is_a_param = false;

        check(key_count > 0, "a level lists at least one param", name);
        check(dup == 0, "no param is listed twice in a level", name);
        check(every_knob_is_a_param, "every knob on a level is that level's own param", name);
        note("level %-9s params %2d  knobs %2d", name, key_count, knob_count);

        if(params_total) *params_total += key_count;
        if(knobs_total)  *knobs_total  += knob_count;
        if(!after) break;
        p = after;
    }
}

/* The whole document: syntax, then what it says about the module. */
static void CheckDocument(const char* name, const char* buf, int len, bool hierarchy)
{
    const bool json_ok = IsJson(buf, len);
    check(json_ok, "the pages parse as JSON", name);
    if(!json_ok) return;
    const char* end = buf + len;
    if(!hierarchy) return;

    int params_total = 0, knobs_total = 0;
    NameSet defined;
    CheckLevels(buf, end, defined, &params_total, &knobs_total);

    check(defined.has("root"), "the root level is there", "root");
    check(defined.n > 0, "the module has levels at all", "");

    /* the pages' cross-references */
    int links = 0, missing_links = 0;
    {
        char level[40];
        const char* p = buf;
        while(true){
            const char* next = StringValue(p, end, "\"level\"", level, sizeof level);
            if(!next) break;
            links++;
            if(!defined.has(level)){
                missing_links++;
                if(missing_links <= 3) note("a page links to a level that is not defined: %s", level);
            }
            p = next;
        }
    }
    check(missing_links == 0, "every page links to a level that exists", "");
    note("%d level(s), %d param slots, %d knobs, %d page link(s)", defined.n, params_total, knobs_total, links);
}

int main(int argc, char** argv)
{
    if(argc > 1 && strcmp(argv[1], "--selftest") == 0){
        /* The incident, verbatim: two stray quotes in the Looper's param list,
         * which the host's parser rejects as one document. */
        static const char* broken =
            "{\"pad_layout\":\"chromatic\",\"levels\":{\"root\":{\"label\":\"CHOMPI\","
            "\"params\":[{\"key\":\"volume\"}],\"knobs\":[\"volume\"]},"
            "\"looper\":{\"label\":\"Looper\",\"params\":[{\"key\":\"looper_start\"},"
            "\"{\"key\":\"looper_end\"},\"{\"key\":\"loop_clear\"}],"
            "\"knobs\":[\"looper_start\",\"looper_end\"]}}}";
        static const char* good =
            "{\"pad_layout\":\"chromatic\",\"levels\":{\"root\":{\"label\":\"CHOMPI\","
            "\"params\":[{\"key\":\"volume\"}],\"knobs\":[\"volume\"]},"
            "\"looper\":{\"label\":\"Looper\",\"params\":[{\"key\":\"looper_start\"},"
            "{\"key\":\"looper_end\"},{\"key\":\"loop_clear\"}],"
            "\"knobs\":[\"looper_start\",\"looper_end\"]}}}";
        const bool bad_rejected  = !IsJson(broken, (int)strlen(broken));
        const bool good_accepted = IsJson(good, (int)strlen(good));
        check(bad_rejected, "the incident's hierarchy is rejected", "two stray quotes");
        check(good_accepted, "the repaired hierarchy is accepted", "");
        printf("\n%s (%d failures)\n", g_fails ? "VERDICT: FAIL" : "VERDICT: PASS", g_fails);
        return g_fails ? 1 : 0;
    }

    const char* so = argc > 1 ? argv[1] : "build/chompi_move.so";
    void* lib = dlopen(so, RTLD_NOW);
    if(!lib){ printf("dlopen(%s): %s\n", so, dlerror()); return 1; }
    auto init = (plugin_api_v2_t* (*)(const host_api_v1_t*))dlsym(lib, "move_plugin_init_v2");
    if(!init){ printf("dlsym move_plugin_init_v2: %s\n", dlerror()); return 1; }
    static host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    static uint8_t mailbox[8192];
    memset(mailbox, 0, sizeof mailbox);
    host.mapped_memory = mailbox;
    host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    host.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    plugin_api_v2_t* api = init(&host);
    void* inst = api->create_instance(argc > 2 ? argv[2] : "module", NULL);

    static char hierarchy[16384];
    static char chain[16384];
    hierarchy[0] = 0;
    chain[0] = 0;
    const int h_len = api->get_param(inst, "ui_hierarchy", hierarchy, (int)sizeof hierarchy);
    const int c_len = api->get_param(inst, "chain_params", chain, (int)sizeof chain);

    printf("ui_hierarchy %d bytes, chain_params %d bytes\n\n", h_len, c_len);
    check(h_len > 0, "the module serves a hierarchy", "");
    check(c_len > 0, "the module serves chain_params", "");
    if(h_len <= 0 || c_len <= 0){ printf("\nVERDICT: FAIL (%d failures)\n", g_fails); return 1; }

    /* Everything a page may point at, from chain_params. */
    NameSet served;
    CollectKeys(chain, chain + c_len, served);

    CheckDocument("chain_params", chain, c_len, false);
    CheckDocument("ui_hierarchy", hierarchy, h_len, true);

    /* Every key a page names must be a param the module actually serves. */
    NameSet named;
    CollectKeys(hierarchy, hierarchy + h_len, named);
    int unknown = 0;
    for(int i = 0; i < named.n; i++){
        if(!served.has(named.v[i])){
            unknown++;
            if(unknown <= 5) note("a page names a param that is not served: %s", named.v[i]);
        }
    }
    check(unknown == 0, "every param a page names is one the module serves", "");
    note("%d param(s) served, %d named by the pages", served.n, named.n);

    printf("\nVERDICT: %s (%d failures)\n", g_fails ? "FAIL" : "PASS", g_fails);
    return g_fails ? 1 : 0;
}
