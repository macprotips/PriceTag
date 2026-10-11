/* sm2fix: a drop-in d3d12.dll proxy for Marvel's Spider-Man 2 that inserts the
 * UAV barrier the game forgets after one compute dispatch.
 *
 * vkd3d-proton (Linux/Proton) fixes this inside its D3D12 implementation with a
 * per-shader quirk (VKD3D_SHADER_QUIRK_FORCE_COMPUTE_BARRIER on shader hash
 * 0x324071d329f05ccc). Apple's D3DMetal (CrossOver / Game Porting Toolkit / Whisky)
 * is closed source, so we do the same thing one layer up: sit between the game and
 * whatever d3d12.dll is installed, hash every compute shader, and after any
 * Dispatch() that uses a listed shader, record a global UAV barrier, which the
 * translation layer must honour.
 *
 * Config: sm2fix.ini next to this DLL, overridden by SM2FIX_* environment variables.
 * Log: sm2fix.log next to this DLL. */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>

#include "sm2fix_core.h"
#include "sm2fix_dxbc.h"
#include "sm2fix_cpu.h"

_Static_assert(sizeof(void *) == 8, "Spider-Man 2 is a 64-bit game; build for x86_64");
_Static_assert(sizeof(D3D12_SHADER_BYTECODE) == 16, "stream CS subobject layout");
_Static_assert(sizeof(D3D12_CACHED_PIPELINE_STATE) == 16, "stream cached PSO layout");
_Static_assert(sizeof(D3D12_STREAM_OUTPUT_DESC) == 32, "stream layout table");
_Static_assert(sizeof(D3D12_BLEND_DESC) == 328, "stream layout table");
_Static_assert(sizeof(D3D12_RASTERIZER_DESC) == 44, "stream layout table");
_Static_assert(sizeof(D3D12_DEPTH_STENCIL_DESC) == 52, "stream layout table");
_Static_assert(sizeof(D3D12_DEPTH_STENCIL_DESC1) == 56, "stream layout table");
_Static_assert(sizeof(D3D12_INPUT_LAYOUT_DESC) == 16, "stream layout table");
_Static_assert(sizeof(DXGI_FORMAT) * 8 + sizeof(UINT) == 36, "stream layout table");
_Static_assert(sizeof(D3D12_VIEW_INSTANCING_DESC) == 24, "stream layout table");

#define VT_INDEX(vtbl_type, member) (offsetof(vtbl_type, member) / sizeof(void *))

static struct sm2_config g_cfg;
static HMODULE g_self, g_real;
static FILE *g_log;
static SRWLOCK g_lock = SRWLOCK_INIT;
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
/* Set at DLL_PROCESS_DETACH: other threads may have died holding a lock, so take none. */
static BOOL g_exiting;
static volatile LONG64 g_barriers, g_target_psos, g_dispatches, g_indirect, g_submits[4], g_waits,
        g_wait_timeouts, g_pso_failures, g_fence_signals, g_fence_waits, g_minmax_static, g_minmax_heap,
        g_samplers_heap, g_rootsigs;

/* ------------------------------------------------------------------------ logging */

static void log_msg(const char *fmt, ...)
{
    va_list args;

    if (!g_log)
        return;
    if (!g_exiting)
        AcquireSRWLockExclusive(&g_lock);
    va_start(args, fmt);
    fprintf(g_log, "[%lu] ", GetTickCount());
    vfprintf(g_log, fmt, args);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(args);
    if (!g_exiting)
        ReleaseSRWLockExclusive(&g_lock);
}

static void path_next_to_self(WCHAR *out, size_t out_len, const WCHAR *file)
{
    WCHAR *slash;

    GetModuleFileNameW(g_self, out, (DWORD)out_len);
    slash = wcsrchr(out, L'\\');
    if (!slash)
        slash = wcsrchr(out, L'/');
    if (slash)
        slash[1] = 0;
    else
        out[0] = 0;
    wcsncat(out, file, out_len - wcslen(out) - 1);
}

/* ------------------------------------------------------------------------- config */

static void load_config(void)
{
    static const char *env_keys[][2] =
    {
        {"SM2FIX_MODE", "mode"},
        {"SM2FIX_HASHES", "hashes"},
        {"SM2FIX_LOG_SHADERS", "log_shaders"},
        {"SM2FIX_REAL_D3D12", "real_d3d12"},
        {"SM2FIX_SPOOF", "spoof"},
        {"SM2FIX_DUMP_COMPUTE", "dump_compute"},
        {"SM2FIX_DUMP_GRAPHICS", "dump_graphics"},
        {"SM2FIX_MINMAX", "minmax"},
        {"SM2FIX_PROFILE", "profile"},
        {"SM2FIX_CPU_COUNT", "cpu_count"},
        {"SM2FIX_PSO_DELAY_MS", "pso_delay_ms"},
        {"SM2FIX_TRACE_QUERIES", "trace_queries"},
        {"SM2FIX_FAKE_TIMESTAMPS", "fake_timestamps"},
        {"SM2FIX_PROBE_SKIN", "probe_skin"},
        {"SM2FIX_PROBE_SKIN_RVA", "probe_skin_rva"},
        {"SM2FIX_PROBE_JOBSYS_RVA", "probe_jobsys_rva"},
        {"SM2FIX_PROBE_SB_RVA", "probe_sb_rva"},
        {"SM2FIX_PROBE_DISPATCH_RVA", "probe_dispatch_rva"},
        {"SM2FIX_SKIN_BUDGET", "skin_budget"},
        {"SM2FIX_SKIP_SKINNING", "skip_skinning"},
        {"SM2FIX_REPLACE_SKINNING", "replace_skinning"},
        {"SM2FIX_SKINNING_HASH", "skinning_hash"},
        {"SM2FIX_SNAPSHOT_MS", "snapshot_ms"},
        {"SM2FIX_FORCE_ANIM", "force_anim"},
        {"SM2FIX_TRAP_WRITER", "trap_writer"},
        {"SM2FIX_MANAGER_RVA", "manager_rva"},
        {"SM2FIX_FORCE_ANIM_DELAY_MS", "force_anim_delay_ms"},
        {"SM2FIX_ANIM_VTBL_RVAS", "anim_vtbl_rvas"},
        {"SM2FIX_ZIVA_CPU", "ziva_cpu"},
        {"SM2FIX_ZIVA_RVAS", "ziva_rvas"},
        {"SM2FIX_CAPTURE", "capture"},
        {"SM2FIX_CAPTURE_AFTER_MS", "capture_after_ms"},
        {"SM2FIX_CAPTURE_HASHES", "capture_hashes"},
    };
    WCHAR path[MAX_PATH];
    char value[1024];
    unsigned int i;
    FILE *f;

    sm2_config_defaults(&g_cfg);

    path_next_to_self(path, MAX_PATH, L"sm2fix.ini");
    if ((f = _wfopen(path, L"rb")))
    {
        static char text[16384];
        size_t n = fread(text, 1, sizeof(text) - 1, f);

        text[n] = 0;
        fclose(f);
        sm2_config_parse_text(&g_cfg, text);
    }

    for (i = 0; i < ARRAYSIZE(env_keys); i++)
    {
        if (GetEnvironmentVariableA(env_keys[i][0], value, sizeof(value)) && value[0])
            sm2_config_set(&g_cfg, env_keys[i][1], value);
    }
}

/* -------------------------------------------------------------- vtable patching */

struct hook_entry
{
    void **vtbl;
    unsigned int index;
    void *orig;
};

static struct hook_entry g_hooks[512];
static unsigned int g_hook_count;

/* Looks up the original function for slot `index` of the vtable `obj` uses. */
static void *orig_fn(void *obj, unsigned int index)
{
    void **vtbl = *(void ***)obj;
    void *fn = NULL;
    unsigned int i;

    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_hook_count; i++)
    {
        if (g_hooks[i].vtbl == vtbl && g_hooks[i].index == index)
        {
            fn = g_hooks[i].orig;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return fn;
}

static void hook_slot(void *obj, unsigned int index, void *hook, const char *name)
{
    void **vtbl = *(void ***)obj;
    DWORD old_protect;
    unsigned int i;

    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_hook_count; i++)
    {
        if (g_hooks[i].vtbl == vtbl && g_hooks[i].index == index)
        {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    /* Never hook an empty slot: there would be no original to call. */
    if (g_hook_count == ARRAYSIZE(g_hooks) || vtbl[index] == hook || !vtbl[index])
    {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (!VirtualProtect(&vtbl[index], sizeof(void *), PAGE_READWRITE, &old_protect))
    {
        ReleaseSRWLockExclusive(&g_lock);
        log_msg("ERROR: VirtualProtect failed for %s (vtbl %p), error %lu", name, vtbl, GetLastError());
        return;
    }
    g_hooks[g_hook_count].vtbl = vtbl;
    g_hooks[g_hook_count].index = index;
    g_hooks[g_hook_count].orig = vtbl[index];
    g_hook_count++;
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void *), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[index], sizeof(void *));
    ReleaseSRWLockExclusive(&g_lock);

    log_msg("hooked %s (vtbl %p slot %u)", name, vtbl, index);
}

/* ----------------------------------------------------------- object tracking */

/* Pipeline states whose compute shader needs a trailing barrier. Stale pointers after
 * a PSO is freed are harmless: worst case we add one unnecessary barrier. */
static void *g_target_pso[256];
static unsigned int g_target_pso_next;

static void add_target_pso(void *pso)
{
    AcquireSRWLockExclusive(&g_lock);
    g_target_pso[g_target_pso_next++ % ARRAYSIZE(g_target_pso)] = pso;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement64(&g_target_psos);
}

static BOOL is_target_pso(void *pso)
{
    BOOL found = FALSE;
    unsigned int i;

    if (!pso)
        return FALSE;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < ARRAYSIZE(g_target_pso); i++)
    {
        if (g_target_pso[i] == pso)
        {
            found = TRUE;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/* Per command list: its type (bundles can't take barriers) and whether the currently
 * bound pipeline is a target. Open addressing keyed by the interface pointer. */
#define LIST_TABLE_SIZE 16384
#define TOMBSTONE ((void *)1)

struct list_state
{
    void *list;
    D3D12_COMMAND_LIST_TYPE type;
    BOOL target_bound;
    void *pso;                  /* currently bound pipeline (profiling) */
    struct root_state *root;    /* compute root bindings (capture) */
};

/* With capture: the compute root signature and root arguments currently set on a list. */
#define ROOT_PARAMS 32
struct root_state
{
    void *rs;
    UINT64 val[ROOT_PARAMS];    /* descriptor table GPU handle or root view GPU address */
    uint32_t consts[ROOT_PARAMS][16];
};

static struct list_state g_lists[LIST_TABLE_SIZE];

static struct list_state *find_list_locked(void *list, BOOL create)
{
    size_t h = ((size_t)list >> 4) * 0x9e3779b97f4a7c15ull;
    struct list_state *tomb = NULL;
    unsigned int i;

    for (i = 0; i < LIST_TABLE_SIZE; i++)
    {
        struct list_state *e = &g_lists[(h + i) % LIST_TABLE_SIZE];

        if (e->list == list)
            return e;
        if (e->list == TOMBSTONE && !tomb)
            tomb = e;
        if (!e->list)
        {
            if (!create)
                return NULL;
            if (!tomb)
                tomb = e;
            break;
        }
    }
    if (!create || !tomb)
        return NULL;
    tomb->list = list;
    tomb->type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    tomb->target_bound = FALSE;
    return tomb;
}

static void list_set_type(void *list, D3D12_COMMAND_LIST_TYPE type)
{
    struct list_state *e;

    AcquireSRWLockExclusive(&g_lock);
    if ((e = find_list_locked(list, TRUE)))
        e->type = type;
    ReleaseSRWLockExclusive(&g_lock);
}

static void list_bind_pso(void *list, void *pso)
{
    BOOL target = is_target_pso(pso);
    struct list_state *e;

    AcquireSRWLockExclusive(&g_lock);
    if ((e = find_list_locked(list, target || g_cfg.profile || g_cfg.capture)))
    {
        e->target_bound = target;
        e->pso = pso;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static BOOL list_needs_barrier(void *list)
{
    struct list_state *e;
    BOOL ret;

    AcquireSRWLockShared(&g_lock);
    e = find_list_locked(list, FALSE);
    if (g_cfg.mode >= SM2_MODE_ALL)
        ret = !e || e->type != D3D12_COMMAND_LIST_TYPE_BUNDLE;
    else
        ret = e && e->target_bound && e->type != D3D12_COMMAND_LIST_TYPE_BUNDLE;
    ReleaseSRWLockShared(&g_lock);
    return ret;
}

static void list_forget(void *list)
{
    struct list_state *e;

    AcquireSRWLockExclusive(&g_lock);
    if ((e = find_list_locked(list, FALSE)))
    {
        e->list = TOMBSTONE;
        if (e->root)
            HeapFree(GetProcessHeap(), 0, e->root);
        e->root = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* Updates a list's root state (capture only). what: 0 reset, 1 root signature, 2 table/view,
 * 3 constants. */
static void root_note(void *list, int what, UINT index, UINT64 val, const void *consts, UINT count, UINT offset)
{
    struct list_state *e;
    struct root_state *r;

    if (!g_cfg.capture)
        return;
    AcquireSRWLockExclusive(&g_lock);
    if ((e = find_list_locked(list, TRUE)))
    {
        if (!e->root)
            e->root = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e->root));
        if ((r = e->root))
        {
            if (what == 0 || (what == 1 && r->rs != (void *)(UINT_PTR)val))
                memset(r, 0, sizeof(*r));
            if (what == 1)
                r->rs = (void *)(UINT_PTR)val;
            else if (what == 2 && index < ROOT_PARAMS)
                r->val[index] = val;
            else if (what == 3 && index < ROOT_PARAMS && offset < 16)
                memcpy(&r->consts[index][offset], consts, 4 * (count < 16 - offset ? count : 16 - offset));
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static BOOL root_get(void *list, struct root_state *out)
{
    struct list_state *e;
    BOOL ok = FALSE;

    AcquireSRWLockShared(&g_lock);
    if ((e = find_list_locked(list, FALSE)) && e->root)
    {
        *out = *e->root;
        ok = TRUE;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

/* -------------------------------------------------------------- profiling */

/* The body-skinning compute shader found by decompiling the game's shaders: reads up to
 * 8 bone influences per vertex and writes posed, quantized vertices to a buffer. */
#define SM2_SKINNING_HASH (g_cfg.skinning_hash)
#define PSO_MAP_SIZE 8192
#define PROF_SIZE 2048

struct pso_hash { void *pso; uint64_t hash; };
struct prof_entry { uint64_t hash; LONG64 dispatches, indirect, groups; };

static struct pso_hash g_pso_hashes[PSO_MAP_SIZE];
static struct prof_entry g_prof[PROF_SIZE];
static SRWLOCK g_prof_lock = SRWLOCK_INIT;
static volatile LONG g_prof_last_tick;

static void pso_hash_set(void *pso, uint64_t hash)
{
    size_t h = ((size_t)pso >> 4) * 0x9e3779b97f4a7c15ull;
    unsigned int i;

    AcquireSRWLockExclusive(&g_prof_lock);
    for (i = 0; i < PSO_MAP_SIZE; i++)
    {
        struct pso_hash *e = &g_pso_hashes[(h + i) % PSO_MAP_SIZE];

        if (!e->pso || e->pso == pso)
        {
            e->pso = pso;
            e->hash = hash;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_prof_lock);
}

static BOOL pso_hash_get(void *pso, uint64_t *hash)
{
    size_t h = ((size_t)pso >> 4) * 0x9e3779b97f4a7c15ull;
    BOOL found = FALSE;
    unsigned int i;

    if (!pso)
        return FALSE;
    AcquireSRWLockShared(&g_prof_lock);
    for (i = 0; i < PSO_MAP_SIZE; i++)
    {
        struct pso_hash *e = &g_pso_hashes[(h + i) % PSO_MAP_SIZE];

        if (e->pso == pso)
        {
            *hash = e->hash;
            found = TRUE;
            break;
        }
        if (!e->pso)
            break;
    }
    ReleaseSRWLockShared(&g_prof_lock);
    return found;
}

static void log_profile(const char *when, unsigned int top)
{
    static struct prof_entry copy[PROF_SIZE];
    struct prof_entry tmp;
    unsigned int i, j, n = 0;
    LONG64 skin_d = 0, skin_i = 0, skin_g = 0;

    if (!g_exiting)
        AcquireSRWLockShared(&g_prof_lock);
    for (i = 0; i < PROF_SIZE; i++)
    {
        if (g_prof[i].hash)
            copy[n++] = g_prof[i];
    }
    if (!g_exiting)
        ReleaseSRWLockShared(&g_prof_lock);
    /* Look the skinning shader up before sorting: the sort moves entries into place
     * after their index has been visited, so checking inside it can miss the entry. */
    for (i = 0; i < n; i++)
    {
        if (copy[i].hash == SM2_SKINNING_HASH)
            skin_d = copy[i].dispatches, skin_i = copy[i].indirect, skin_g = copy[i].groups;
    }
    for (i = 0; i < n; i++)
    {
        for (j = i + 1; j < n; j++)
        {
            if (copy[j].dispatches + copy[j].indirect > copy[i].dispatches + copy[i].indirect)
                tmp = copy[i], copy[i] = copy[j], copy[j] = tmp;
        }
    }
    log_msg("profile (%s): %u compute shaders used; SKINNING shader 3d50acdb571dce22: dispatches=%lld"
            " indirect=%lld groups=%lld", when, n, (long long)skin_d, (long long)skin_i, (long long)skin_g);
    for (i = 0; i < n && i < top; i++)
        log_msg("  %016llx dispatches=%lld indirect=%lld groups=%lld", (unsigned long long)copy[i].hash,
                (long long)copy[i].dispatches, (long long)copy[i].indirect, (long long)copy[i].groups);
}

static void profile_dispatch(void *list, BOOL indirect, UINT64 groups)
{
    size_t h;
    struct list_state *e;
    uint64_t hash;
    void *pso = NULL;
    unsigned int i;
    LONG now, last;

    AcquireSRWLockShared(&g_lock);
    if ((e = find_list_locked(list, FALSE)))
        pso = e->pso;
    ReleaseSRWLockShared(&g_lock);
    if (!pso_hash_get(pso, &hash))
        return;

    h = (size_t)(hash * 0x9e3779b97f4a7c15ull);
    AcquireSRWLockExclusive(&g_prof_lock);
    for (i = 0; i < PROF_SIZE; i++)
    {
        struct prof_entry *p = &g_prof[(h + i) % PROF_SIZE];

        if (!p->hash || p->hash == hash)
        {
            p->hash = hash;
            if (indirect)
                p->indirect++;
            else
                p->dispatches++;
            p->groups += groups;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_prof_lock);

    /* Every 20 seconds, snapshot the counters so the log shows activity over time. */
    now = (LONG)GetTickCount();
    last = g_prof_last_tick;
    if (now - last > 20000 && InterlockedCompareExchange(&g_prof_last_tick, now, last) == last)
        log_profile("periodic", 8);
}

static void dump_shader_ex(const char *stage, const void *code, size_t size, BOOL quiet, LONG limit);

static volatile LONG64 g_pso_created;

/* Imitates a cold shader cache: the one launch that came up fixed was the first after
 * a settings change, when every pipeline had to be compiled and loading was slow. */
static void pso_creation_delay(void)
{
    LONG64 n = InterlockedIncrement64(&g_pso_created);

    if (g_cfg.pso_delay_ms)
        Sleep(g_cfg.pso_delay_ms);
    if (n == 1 || !(n & (n - 1)))
        log_msg("pipelines created so far: %lld", (long long)n);
}

/* ------------------------------------------------------------- shader matching */

static void note_compute_shader(void *pso, const void *code, size_t size, const char *how)
{
    uint64_t hash;

    if (!pso || !code || !size)
        return;
    hash = sm2_shader_hash(code, size);
    if (g_cfg.profile || g_cfg.capture)
        pso_hash_set(pso, hash);
    if (hash == SM2_SKINNING_HASH)
        log_msg("skinning shader created via %s -> pso %p", how, pso);
    if (g_cfg.dump_compute)
        dump_shader_ex("cs", code, size, TRUE, 20000);
    if (g_cfg.log_shaders)
        log_msg("compute shader %016llx (%zu bytes) via %s -> pso %p",
                (unsigned long long)hash, size, how, pso);
    if (g_cfg.mode == SM2_MODE_HASH && sm2_config_has_hash(&g_cfg, hash))
    {
        add_target_pso(pso);
        log_msg("TARGET shader %016llx created via %s -> pso %p (barrier will be forced after its dispatches)",
                (unsigned long long)hash, how, pso);
    }
}

/* ------------------------------------------------------------- diagnostics */

static BOOL readable(const void *p, size_t size)
{
    MEMORY_BASIC_INFORMATION info;
    const uint8_t *cur = p, *end = (const uint8_t *)p + size;

    if (!p || !size || size > 64 * 1024 * 1024)
        return FALSE;
    while (cur < end)
    {
        if (!VirtualQuery(cur, &info, sizeof(info)) || info.State != MEM_COMMIT
                || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return FALSE;
        cur = (const uint8_t *)info.BaseAddress + info.RegionSize;
    }
    return TRUE;
}

/* Saves shader bytecode to sm2fix-dumps\<hash>_<stage>.dxil so it can be disassembled. */
static void dump_shader_ex(const char *stage, const void *code, size_t size, BOOL quiet, LONG limit)
{
    static volatile LONG dumps;
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    uint64_t hash;
    FILE *f;

    if (!readable(code, size))
    {
        log_msg("  %s: bytecode %p (%zu bytes) not readable", stage, code, size);
        return;
    }
    hash = sm2_shader_hash(code, size);
    if (!quiet)
        log_msg("  %s shader %016llx (%zu bytes)", stage, (unsigned long long)hash, size);
    if (InterlockedIncrement(&dumps) > limit)
        return;
    path_next_to_self(dir, MAX_PATH, L"sm2fix-dumps");
    CreateDirectoryW(dir, NULL);
    _snwprintf(path, MAX_PATH, L"%ls\\%016llx_%hs.dxil", dir, (unsigned long long)hash, stage);
    path[MAX_PATH - 1] = 0;
    if ((f = _wfopen(path, L"wb")))
    {
        fwrite(code, 1, size, f);
        fclose(f);
    }
}

static void dump_shader(const char *stage, const void *code, size_t size)
{
    dump_shader_ex(stage, code, size, FALSE, 200);
}

static const char *stage_name(uint32_t type)
{
    static const char *names[] = {"rs", "vs", "ps", "ds", "hs", "gs", "cs"};

    if (type < ARRAYSIZE(names))
        return names[type];
    return type == 24 ? "as" : type == 25 ? "ms" : "unknown";
}

static void dump_stream_shader(void *ctx, uint32_t type, const void *code, size_t len)
{
    if (code && len)
        dump_shader(stage_name(type), code, len);
}

static void dump_stream_vertex_shader(void *ctx, uint32_t type, const void *code, size_t len)
{
    if ((type == 1 || type == 25) && code && len)
        dump_shader_ex(stage_name(type), code, len, TRUE, 20000);
}

static void log_hex(const char *what, const void *data, UINT size)
{
    char buf[64 * 3 + 1];
    const uint8_t *p = data;
    UINT i, n = size < 64 ? size : 64;

    if (!readable(data, size ? size : 1))
        return;
    for (i = 0; i < n; i++)
        sprintf(buf + i * 3, "%02x ", p[i]);
    buf[n * 3] = 0;
    log_msg("%s: %s%s", what, buf, size > 64 ? "..." : "");
}

/* ---------------------------------------------------------- command list hooks */

#define CL_IDX(m) VT_INDEX(ID3D12GraphicsCommandListVtbl, m)

/* What D3D12 sees while the game's skin dispatch function runs (see install_skin_budget):
 * set by hk_skin_dispatch on its own thread, read by the command-list hooks below. */
static volatile DWORD g_skin_window_tid;
static volatile LONG64 g_win_set_pso, g_win_set_pso_skin, g_win_dispatch, g_win_dispatch_skin,
        g_win_indirect;
static volatile uint64_t g_win_last_hash;
static volatile UINT g_win_last_x;

static BOOL in_skin_window(void)
{
    return g_skin_window_tid && g_skin_window_tid == GetCurrentThreadId();
}

/* Root signatures by object, so the one the skinning shader runs with can be logged. */
#define ROOTSIG_MAX 256
static struct { void *obj; void *blob; SIZE_T size; } g_rootsig_reg[ROOTSIG_MAX];
static volatile LONG g_rootsig_reg_n, g_win_root_lines;
static volatile LONG64 g_skin_skipped;

static void rootsig_remember(void *obj, const void *blob, SIZE_T size)
{
    LONG i;
    void *copy;

    if (!obj || !size || size > 64 * 1024 || !(copy = HeapAlloc(GetProcessHeap(), 0, size)))
        return;
    memcpy(copy, blob, size);
    if ((i = InterlockedIncrement(&g_rootsig_reg_n) - 1) >= ROOTSIG_MAX)
    {
        HeapFree(GetProcessHeap(), 0, copy);
        return;
    }
    g_rootsig_reg[i].blob = copy;
    g_rootsig_reg[i].size = size;
    g_rootsig_reg[i].obj = obj;
}

/* Up to 80 lines of root-binding detail for the first skin dispatches with work. */
static volatile LONG g_view_window_log;   /* this skin window is a periodic snapshot */

static BOOL win_root_log(void)
{
    if (!in_skin_window())
        return FALSE;
    if (g_view_window_log)
        return TRUE;
    return g_win_root_lines < 80 && InterlockedIncrement(&g_win_root_lines) <= 80;
}

static void log_blob_hex(const char *what, const uint8_t *p, SIZE_T size)
{
    char line[32 * 2 + 1];
    SIZE_T off, i;

    log_msg("%s (%zu bytes):", what, (size_t)size);
    for (off = 0; off < size && off < 4096; off += 32)
    {
        for (i = 0; i < 32 && off + i < size; i++)
            sprintf(line + i * 2, "%02x", p[off + i]);
        log_msg("  %04zx %s", (size_t)off, line);
    }
}

static void STDMETHODCALLTYPE hk_list_SetComputeRootSignature(ID3D12GraphicsCommandList *list, ID3D12RootSignature *rs)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12RootSignature *)
            = orig_fn(list, CL_IDX(SetComputeRootSignature));
    static volatile LONG dumped;
    LONG i, n;

    orig(list, rs);
    root_note(list, 1, 0, (UINT64)(UINT_PTR)rs, NULL, 0, 0);
    if (!win_root_log())
        return;
    log_msg("skin window: SetComputeRootSignature %p", (void *)rs);
    n = g_rootsig_reg_n < ROOTSIG_MAX ? g_rootsig_reg_n : ROOTSIG_MAX;
    for (i = 0; i < n; i++)
    {
        if (g_rootsig_reg[i].obj == rs && !InterlockedExchange(&dumped, 1))
            log_blob_hex("skin window: root signature blob", g_rootsig_reg[i].blob, g_rootsig_reg[i].size);
    }
}

static void log_table_views(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base);
static void *volatile g_bone_res;           /* bone palette buffer (t0) seen in a skin window */
static UINT64 g_bone_width, g_bone_first;
static PVOID g_trap_handler;
static void arm_writer_trap(void);

static void STDMETHODCALLTYPE hk_list_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList *list, UINT index,
        D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)
            = orig_fn(list, CL_IDX(SetComputeRootDescriptorTable));

    orig(list, index, base);
    root_note(list, 2, index, base.ptr, NULL, 0, 0);
    if (win_root_log())
    {
        log_msg("skin window: root %u = descriptor table %#llx", index, (unsigned long long)base.ptr);
        log_table_views(index, base);
    }
}

static void STDMETHODCALLTYPE hk_list_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList *list, UINT index,
        UINT count, const void *data, UINT offset)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, UINT, const void *, UINT)
            = orig_fn(list, CL_IDX(SetComputeRoot32BitConstants));

    orig(list, index, count, data, offset);
    root_note(list, 3, index, 0, data, count, offset);
    if (win_root_log())
    {
        char what[96];

        snprintf(what, sizeof(what), "skin window: root %u = %u constants at %u", index, count, offset);
        log_hex(what, data, count * 4);
    }
}

static void STDMETHODCALLTYPE hk_list_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList *list, UINT index,
        UINT value, UINT offset)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, UINT, UINT)
            = orig_fn(list, CL_IDX(SetComputeRoot32BitConstant));

    orig(list, index, value, offset);
    root_note(list, 3, index, 0, &value, 1, offset);
    if (win_root_log())
        log_msg("skin window: root %u = constant %#x at %u", index, value, offset);
}

#define ROOT_VIEW_HOOK(name, label) \
static void STDMETHODCALLTYPE hk_list_##name(ID3D12GraphicsCommandList *list, UINT index, \
        D3D12_GPU_VIRTUAL_ADDRESS va) \
{ \
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS) \
            = orig_fn(list, CL_IDX(name)); \
\
    orig(list, index, va); \
    root_note(list, 2, index, va, NULL, 0, 0); \
    if (win_root_log()) \
        log_msg("skin window: root %u = " label " %#llx", index, (unsigned long long)va); \
}
ROOT_VIEW_HOOK(SetComputeRootConstantBufferView, "CBV")
ROOT_VIEW_HOOK(SetComputeRootShaderResourceView, "SRV")
ROOT_VIEW_HOOK(SetComputeRootUnorderedAccessView, "UAV")


static void hook_command_list_object(void *list);
static void capture_drop_list(void *list);

static ULONG STDMETHODCALLTYPE hk_list_Release(ID3D12GraphicsCommandList *list)
{
    ULONG (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *) = orig_fn(list, CL_IDX(Release));
    ULONG refs = orig(list);

    if (!refs)
        list_forget(list);
    return refs;
}

static HRESULT STDMETHODCALLTYPE hk_list_Reset(ID3D12GraphicsCommandList *list,
        ID3D12CommandAllocator *allocator, ID3D12PipelineState *pso)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *,
            ID3D12PipelineState *) = orig_fn(list, CL_IDX(Reset));
    HRESULT hr = orig(list, allocator, pso);

    if (SUCCEEDED(hr))
    {
        list_bind_pso(list, pso);
        root_note(list, 0, 0, 0, NULL, 0, 0);
        capture_drop_list(list);
    }
    return hr;
}

static void STDMETHODCALLTYPE hk_list_ClearState(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12PipelineState *)
            = orig_fn(list, CL_IDX(ClearState));

    orig(list, pso);
    list_bind_pso(list, pso);
    root_note(list, 0, 0, 0, NULL, 0, 0);
}

static void STDMETHODCALLTYPE hk_list_SetPipelineState(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12PipelineState *)
            = orig_fn(list, CL_IDX(SetPipelineState));

    orig(list, pso);
    list_bind_pso(list, pso);
    if (in_skin_window())
    {
        uint64_t hash = 0;

        InterlockedIncrement64(&g_win_set_pso);
        if (pso_hash_get(pso, &hash))
            g_win_last_hash = hash;
        if (hash == SM2_SKINNING_HASH)
            InterlockedIncrement64(&g_win_set_pso_skin);
    }
}

static BOOL list_bound_hash(void *list, uint64_t *hash)
{
    struct list_state *e;
    void *pso = NULL;

    AcquireSRWLockShared(&g_lock);
    if ((e = find_list_locked(list, FALSE)))
        pso = e->pso;
    ReleaseSRWLockShared(&g_lock);
    return pso_hash_get(pso, hash);
}

static BOOL capture_wanted(uint64_t hash);
static void capture_dispatch(ID3D12GraphicsCommandList *list, uint64_t hash, UINT x, UINT y, UINT z,
        void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, UINT, UINT));

static void STDMETHODCALLTYPE hk_list_Dispatch(ID3D12GraphicsCommandList *list, UINT x, UINT y, UINT z)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, UINT, UINT)
            = orig_fn(list, CL_IDX(Dispatch));
    uint64_t bound = 0;

    if (g_cfg.skip_skinning && list_bound_hash(list, &bound) && bound == SM2_SKINNING_HASH)
    {
        LONG64 n = InterlockedIncrement64(&g_skin_skipped);

        if (n == 1 || !(n & (n - 1)))
            log_msg("skip_skinning: dropped %lld skinning dispatches so far (this one x=%u)", (long long)n, x);
        return;
    }
    if (g_cfg.capture && x && y && z && list_bound_hash(list, &bound) && capture_wanted(bound))
    {
        capture_dispatch(list, bound, x, y, z, orig);
        bound = 0;
    }
    else
        orig(list, x, y, z);
    InterlockedIncrement64(&g_dispatches);
    if (in_skin_window())
    {
        uint64_t hash = 0;

        InterlockedIncrement64(&g_win_dispatch);
        g_win_last_x = x;
        if (list_bound_hash(list, &hash) && hash == SM2_SKINNING_HASH)
            InterlockedIncrement64(&g_win_dispatch_skin);
        if (win_root_log())
            log_msg("skin window: Dispatch(%u, %u, %u) on list %p", x, y, z, (void *)list);
    }
    if (g_cfg.profile)
        profile_dispatch(list, FALSE, (UINT64)x * y * z);

    if (g_cfg.mode != SM2_MODE_OFF && list_needs_barrier(list))
    {
        D3D12_RESOURCE_BARRIER barrier;
        LONG64 n;

        /* A NULL-resource UAV barrier: every UAV write before it completes before any
         * UAV access after it. Same intent as vkd3d-proton's forced compute barrier. */
        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = NULL;
        list->lpVtbl->ResourceBarrier(list, 1, &barrier);

        n = InterlockedIncrement64(&g_barriers);
        if (n == 1 || !(n & (n - 1)))
            log_msg("forced barriers so far: %lld", (long long)n);
    }
}

static void STDMETHODCALLTYPE hk_list_ExecuteIndirect(ID3D12GraphicsCommandList *list,
        ID3D12CommandSignature *signature, UINT max_count, ID3D12Resource *args, UINT64 args_offset,
        ID3D12Resource *count_buffer, UINT64 count_offset)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *,
            UINT64, ID3D12Resource *, UINT64) = orig_fn(list, CL_IDX(ExecuteIndirect));

    orig(list, signature, max_count, args, args_offset, count_buffer, count_offset);
    InterlockedIncrement64(&g_indirect);
    if (in_skin_window())
        InterlockedIncrement64(&g_win_indirect);
    if (g_cfg.profile)
        profile_dispatch(list, TRUE, max_count);

    /* Indirect dispatches skip Dispatch(), so cover them too in the diagnostic modes. */
    if (g_cfg.mode >= SM2_MODE_ALL && list_needs_barrier(list))
    {
        D3D12_RESOURCE_BARRIER barrier;

        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->lpVtbl->ResourceBarrier(list, 1, &barrier);
        InterlockedIncrement64(&g_barriers);
    }
}

/* ------------------------------------------------------- readback monitor */

/* With probe_skin: every buffer the GPU copies into a READBACK heap is something the CPU
 * reads back. A D3DMetal bug there (like the all-zero timestamps) would feed the game's
 * CPU logic bad data. Track each readback destination (with a reference held so it can
 * be mapped later) and, at each skin snapshot, log how much of it is non-zero and whether
 * it changed since the last snapshot. */
#define READBACK_MAX 128
struct readback_rec
{
    ID3D12Resource *dst;
    ID3D12Resource *src;        /* last source (not referenced) */
    UINT64 width, src_width;
    unsigned int src_heap, kind; /* kind: 1 buffer copy, 2 resource copy, 3 texture copy */
    volatile LONG64 copies;
    uint64_t last_hash;
    unsigned int snapshots, changes;
};
static struct readback_rec g_readbacks[READBACK_MAX];
static volatile LONG g_readback_count;
static SRWLOCK g_readback_lock = SRWLOCK_INIT;

static unsigned int heap_type_of(ID3D12Resource *res)
{
    D3D12_HEAP_PROPERTIES props;
    D3D12_HEAP_FLAGS flags;

    return res && SUCCEEDED(res->lpVtbl->GetHeapProperties(res, &props, &flags)) ? (unsigned int)props.Type : 0;
}

static void note_readback(ID3D12Resource *dst, ID3D12Resource *src, unsigned int kind)
{
    LONG i, n;

    if (!dst || heap_type_of(dst) != D3D12_HEAP_TYPE_READBACK)
        return;
    AcquireSRWLockExclusive(&g_readback_lock);
    n = g_readback_count;
    for (i = 0; i < n; i++)
        if (g_readbacks[i].dst == dst)
            break;
    if (i == n && n < READBACK_MAX)
    {
        D3D12_RESOURCE_DESC desc;

        dst->lpVtbl->AddRef(dst);
        dst->lpVtbl->GetDesc(dst, &desc);
        g_readbacks[i].dst = dst;
        g_readbacks[i].width = desc.Width;
        g_readback_count = n + 1;
    }
    if (i < READBACK_MAX && g_readbacks[i].dst == dst)
    {
        struct readback_rec *r = &g_readbacks[i];

        r->kind = kind;
        if (src && r->src != src)
        {
            D3D12_RESOURCE_DESC desc;

            src->lpVtbl->GetDesc(src, &desc);
            r->src = src;
            r->src_width = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? desc.Width : desc.Width * desc.Height;
            r->src_heap = heap_type_of(src);
        }
        r->copies++;
    }
    ReleaseSRWLockExclusive(&g_readback_lock);
}

static void log_readbacks(void)
{
    LONG i, n = g_readback_count;

    log_msg("readbacks: %ld GPU->CPU buffers seen", (long)n);
    for (i = 0; i < n; i++)
    {
        struct readback_rec *r = &g_readbacks[i];
        D3D12_RANGE range = {0, (SIZE_T)r->width}, none = {0, 0};
        const uint8_t *map = NULL;
        UINT64 len = r->width < (64u << 20) ? r->width : (64u << 20), k, nonzero = 0;
        uint64_t h = 0xcbf29ce484222325ull;

        if (!r->dst || FAILED(r->dst->lpVtbl->Map(r->dst, 0, &range, (void **)&map)) || !map)
            continue;
        for (k = 0; k + 8 <= len; k += 8)
        {
            uint64_t v = *(const uint64_t *)(map + k);

            nonzero += v != 0;
            h = (h ^ v) * 0x100000001b3ull;
        }
        r->dst->lpVtbl->Unmap(r->dst, 0, &none);
        r->snapshots++;
        if (r->snapshots > 1 && h != r->last_hash)
            r->changes++;
        r->last_hash = h;
        log_msg("readbacks: [%ld] %llu bytes, %lld copies (%s) from src %p (%llu, heap %u): %.1f%% non-zero, changed in %u of %u snapshots",
                (long)i, (unsigned long long)r->width, (long long)r->copies,
                r->kind == 1 ? "buffer" : r->kind == 2 ? "resource" : "texture", (void *)r->src,
                (unsigned long long)r->src_width, r->src_heap, len ? 100.0 * nonzero * 8 / len : 0.0,
                r->changes, r->snapshots > 1 ? r->snapshots - 1 : 0);
    }
}

static void STDMETHODCALLTYPE hk_list_CopyBufferRegion(ID3D12GraphicsCommandList *list, ID3D12Resource *dst,
        UINT64 dst_off, ID3D12Resource *src, UINT64 src_off, UINT64 n)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64,
            UINT64) = orig_fn(list, CL_IDX(CopyBufferRegion));

    orig(list, dst, dst_off, src, src_off, n);
    note_readback(dst, src, 1);
}

static void STDMETHODCALLTYPE hk_list_CopyResource(ID3D12GraphicsCommandList *list, ID3D12Resource *dst,
        ID3D12Resource *src)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *)
            = orig_fn(list, CL_IDX(CopyResource));

    orig(list, dst, src);
    note_readback(dst, src, 2);
}

static void STDMETHODCALLTYPE hk_list_CopyTextureRegion(ID3D12GraphicsCommandList *list,
        const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src,
        const D3D12_BOX *box)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT,
            const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *) = orig_fn(list, CL_IDX(CopyTextureRegion));

    orig(list, dst, x, y, z, src, box);
    if (dst && dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        note_readback(dst->pResource, src ? src->pResource : NULL, 3);
}

/* --------------------------------------------------------- GPU query tracing */

/* The engine reads GPU answers back on the CPU (timestamps for budgets, occlusion for
 * visibility). If D3DMetal returns bad values on M1-M4, the animation-LOD system could
 * decide nothing needs animating. Log what the game asks for and what it gets. */
static volatile LONG64 g_query_begin[8], g_query_end[8], g_query_resolve[8];
static volatile LONG g_query_last_tick;
static UINT64 g_ts_freq;

struct resolve_rec
{
    ID3D12Resource *dest;
    UINT64 offset;
    UINT count;
    D3D12_QUERY_TYPE type;
    const uint8_t *mapped;
};
static struct resolve_rec g_resolves[48];
static unsigned int g_resolve_count;

static const char *query_type_name(D3D12_QUERY_TYPE t)
{
    static const char *names[] = {"occlusion", "binary-occlusion", "timestamp", "pipeline-stats",
            "so-stats0", "so-stats1", "so-stats2", "so-stats3"};
    return (unsigned int)t < 8 ? names[t] : "other";
}

/* ------------------------------------------------------ timestamp emulation */

/* On M1-M4, D3DMetal reports a 60 Hz timestamp frequency and resolves every timestamp
 * query to 0. With fake_timestamps, the shim answers instead: QueryPerformanceCounter
 * frequency, and the CPU time at which each EndQuery(TIMESTAMP) was recorded, written
 * straight into the game's (CPU-visible) resolve buffer. */
struct ts_heap { ID3D12QueryHeap *heap; volatile UINT64 *values; UINT count; };
struct ts_dest { ID3D12Resource *res; uint8_t *mapped; };
static struct ts_heap g_ts_heaps[16];
static struct ts_dest g_ts_dests[64];
static volatile LONG64 g_ts_faked_resolves, g_ts_fallback_resolves;

static UINT64 qpc_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (UINT64)t.QuadPart;
}

static UINT64 qpc_freq(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (UINT64)f.QuadPart;
}

static void ts_register_heap(ID3D12QueryHeap *heap, UINT count)
{
    volatile UINT64 *values = VirtualAlloc(NULL, (SIZE_T)count * sizeof(UINT64), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned int i;

    if (!values)
    {
        log_msg("fake_timestamps: could not allocate %u slots", count);
        return;
    }
    AcquireSRWLockExclusive(&g_prof_lock);
    for (i = 0; i < ARRAYSIZE(g_ts_heaps); i++)
    {
        if (!g_ts_heaps[i].heap)
        {
            g_ts_heaps[i].heap = heap;
            g_ts_heaps[i].values = values;
            g_ts_heaps[i].count = count;
            values = NULL;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_prof_lock);
    if (values)
        VirtualFree((void *)values, 0, MEM_RELEASE);
    else
        log_msg("fake_timestamps: emulating timestamp heap %p (%u slots)", (void *)heap, count);
}

static struct ts_heap *ts_find_heap(ID3D12QueryHeap *heap)
{
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(g_ts_heaps); i++)
    {
        if (g_ts_heaps[i].heap == heap)
            return &g_ts_heaps[i];
    }
    return NULL;
}

/* CPU pointer for a resolve destination (mapped once and kept), or NULL. */
static uint8_t *ts_map_dest(ID3D12Resource *res)
{
    uint8_t *ptr = NULL;
    unsigned int i;

    AcquireSRWLockExclusive(&g_prof_lock);
    for (i = 0; i < ARRAYSIZE(g_ts_dests); i++)
    {
        if (g_ts_dests[i].res == res)
        {
            ptr = g_ts_dests[i].mapped;
            break;
        }
        if (!g_ts_dests[i].res)
        {
            void *p = NULL;

            res->lpVtbl->AddRef(res);
            if (SUCCEEDED(res->lpVtbl->Map(res, 0, NULL, &p)) && p)
            {
                g_ts_dests[i].res = res;
                g_ts_dests[i].mapped = ptr = p;
            }
            else
                res->lpVtbl->Release(res);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_prof_lock);
    return ptr;
}

static void STDMETHODCALLTYPE hk_list_BeginQuery(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type, UINT index)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT)
            = orig_fn(list, CL_IDX(BeginQuery));
    orig(list, heap, type, index);
    if ((unsigned int)type < 8)
        InterlockedIncrement64(&g_query_begin[type]);
}

static void STDMETHODCALLTYPE hk_list_EndQuery(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type, UINT index)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT)
            = orig_fn(list, CL_IDX(EndQuery));

    if (g_cfg.fake_timestamps && type == D3D12_QUERY_TYPE_TIMESTAMP)
    {
        struct ts_heap *h = ts_find_heap(heap);

        if (h && index < h->count)
            h->values[index] = qpc_now();
    }
    orig(list, heap, type, index);
    if ((unsigned int)type < 8)
        InterlockedIncrement64(&g_query_end[type]);
}

static void STDMETHODCALLTYPE hk_list_ResolveQueryData(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource *dest, UINT64 offset)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT, UINT,
            ID3D12Resource *, UINT64) = orig_fn(list, CL_IDX(ResolveQueryData));
    unsigned int i;
    BOOL known = FALSE, faked = FALSE;

    if (g_cfg.fake_timestamps && type == D3D12_QUERY_TYPE_TIMESTAMP && dest)
    {
        struct ts_heap *h = ts_find_heap(heap);
        uint8_t *mapped;

        if (h && (UINT64)start + count <= h->count && (mapped = ts_map_dest(dest)))
        {
            /* Write our values now and skip the GPU resolve, which would overwrite them
             * with D3DMetal's zeros. */
            memcpy(mapped + offset, (const void *)(h->values + start), (size_t)count * sizeof(UINT64));
            faked = TRUE;
            if (InterlockedIncrement64(&g_ts_faked_resolves) == 1)
                log_msg("fake_timestamps: first resolve answered by the shim (%u values)", count);
        }
        else if (InterlockedIncrement64(&g_ts_fallback_resolves) <= 8)
            log_msg("fake_timestamps: couldn't emulate resolve (heap %p known=%d, dest %p): passing through",
                    (void *)heap, h != NULL, (void *)dest);
    }
    if (!faked)
        orig(list, heap, type, start, count, dest, offset);
    if ((unsigned int)type < 8)
        InterlockedIncrement64(&g_query_resolve[type]);
    if (!g_cfg.trace_queries)
        return;
    if (!dest || !count)
        return;

    AcquireSRWLockExclusive(&g_prof_lock);
    for (i = 0; i < g_resolve_count; i++)
    {
        if (g_resolves[i].dest == dest && g_resolves[i].offset == offset && g_resolves[i].type == type)
        {
            known = TRUE;
            break;
        }
    }
    if (!known && g_resolve_count < ARRAYSIZE(g_resolves))
    {
        struct resolve_rec *r = &g_resolves[g_resolve_count];
        D3D12_RANGE none = {0, 0};
        void *ptr = NULL;

        /* Keep our own reference and mapping so we can read the values any time later. */
        dest->lpVtbl->AddRef(dest);
        if (SUCCEEDED(dest->lpVtbl->Map(dest, 0, NULL, &ptr)) && ptr)
        {
            r->dest = dest;
            r->offset = offset;
            r->count = count;
            r->type = type;
            r->mapped = ptr;
            g_resolve_count++;
        }
        else
            dest->lpVtbl->Release(dest);
        (void)none;
    }
    ReleaseSRWLockExclusive(&g_prof_lock);
    if (!known)
        log_msg("ResolveQueryData type=%s start=%u count=%u -> resource %p +%llu", query_type_name(type), start, count,
                (void *)dest, (unsigned long long)offset);
}

static void dump_query_values(void)
{
    unsigned int i, j, n;

    AcquireSRWLockShared(&g_prof_lock);
    n = g_resolve_count;
    ReleaseSRWLockShared(&g_prof_lock);
    log_msg("queries so far: timestamp end=%lld resolve=%lld | occlusion begin=%lld resolve=%lld | binary-occl begin=%lld"
            " | pipeline-stats begin=%lld; timestamp freq=%llu",
            (long long)g_query_end[2], (long long)g_query_resolve[2], (long long)g_query_begin[0],
            (long long)g_query_resolve[0], (long long)g_query_begin[1], (long long)g_query_begin[3],
            (unsigned long long)g_ts_freq);
    for (i = 0; i < n; i++)
    {
        const struct resolve_rec *r = &g_resolves[i];
        const UINT64 *v = (const UINT64 *)(r->mapped + r->offset);
        char line[512];
        size_t len = 0;
        UINT shown = r->count < 8 ? r->count : 8;

        if (r->type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS)
        {
            /* IAVertices IAPrimitives VSInvocations GSInvocations GSPrimitives CInvocations CPrimitives
             * PSInvocations HSInvocations DSInvocations CSInvocations */
            for (j = 0; j < 11; j++)
                len += snprintf(line + len, sizeof(line) - len, "%llu ", (unsigned long long)v[j]);
        }
        else if (r->type == D3D12_QUERY_TYPE_TIMESTAMP && g_ts_freq)
        {
            len += snprintf(line + len, sizeof(line) - len, "t0=%llu deltas(us):", (unsigned long long)v[0]);
            for (j = 1; j < shown; j++)
                len += snprintf(line + len, sizeof(line) - len, " %.1f",
                        (double)(INT64)(v[j] - v[j - 1]) * 1e6 / (double)g_ts_freq);
        }
        else
        {
            for (j = 0; j < shown; j++)
                len += snprintf(line + len, sizeof(line) - len, "%llu ", (unsigned long long)v[j]);
        }
        log_msg("  %s x%u @%p+%llu: %s", query_type_name(r->type), r->count, (void *)r->dest,
                (unsigned long long)r->offset, line);
    }
}

static void maybe_dump_queries(void)
{
    LONG now = (LONG)GetTickCount(), last = g_query_last_tick;

    if (now - last > 15000 && InterlockedCompareExchange(&g_query_last_tick, now, last) == last)
        dump_query_values();
}

static void hook_command_list_vtable(void *list)
{
    if (g_cfg.trace_queries || g_cfg.fake_timestamps)
    {
        hook_slot(list, CL_IDX(BeginQuery), (void *)hk_list_BeginQuery, "CommandList::BeginQuery");
        hook_slot(list, CL_IDX(EndQuery), (void *)hk_list_EndQuery, "CommandList::EndQuery");
        hook_slot(list, CL_IDX(ResolveQueryData), (void *)hk_list_ResolveQueryData, "CommandList::ResolveQueryData");
    }
    hook_slot(list, CL_IDX(ExecuteIndirect), (void *)hk_list_ExecuteIndirect, "CommandList::ExecuteIndirect");
    hook_slot(list, CL_IDX(Release), (void *)hk_list_Release, "CommandList::Release");
    hook_slot(list, CL_IDX(Reset), (void *)hk_list_Reset, "CommandList::Reset");
    hook_slot(list, CL_IDX(ClearState), (void *)hk_list_ClearState, "CommandList::ClearState");
    hook_slot(list, CL_IDX(SetPipelineState), (void *)hk_list_SetPipelineState, "CommandList::SetPipelineState");
    hook_slot(list, CL_IDX(Dispatch), (void *)hk_list_Dispatch, "CommandList::Dispatch");
    if (g_cfg.probe_skin)
    {
        hook_slot(list, CL_IDX(CopyBufferRegion), (void *)hk_list_CopyBufferRegion, "CommandList::CopyBufferRegion");
        hook_slot(list, CL_IDX(CopyResource), (void *)hk_list_CopyResource, "CommandList::CopyResource");
        hook_slot(list, CL_IDX(CopyTextureRegion), (void *)hk_list_CopyTextureRegion, "CommandList::CopyTextureRegion");
    }
    if (g_cfg.probe_skin || g_cfg.capture)
    {
        hook_slot(list, CL_IDX(SetComputeRootSignature), (void *)hk_list_SetComputeRootSignature,
                "CommandList::SetComputeRootSignature");
        hook_slot(list, CL_IDX(SetComputeRootDescriptorTable), (void *)hk_list_SetComputeRootDescriptorTable,
                "CommandList::SetComputeRootDescriptorTable");
        hook_slot(list, CL_IDX(SetComputeRoot32BitConstants), (void *)hk_list_SetComputeRoot32BitConstants,
                "CommandList::SetComputeRoot32BitConstants");
        hook_slot(list, CL_IDX(SetComputeRoot32BitConstant), (void *)hk_list_SetComputeRoot32BitConstant,
                "CommandList::SetComputeRoot32BitConstant");
        hook_slot(list, CL_IDX(SetComputeRootConstantBufferView), (void *)hk_list_SetComputeRootConstantBufferView,
                "CommandList::SetComputeRootConstantBufferView");
        hook_slot(list, CL_IDX(SetComputeRootShaderResourceView), (void *)hk_list_SetComputeRootShaderResourceView,
                "CommandList::SetComputeRootShaderResourceView");
        hook_slot(list, CL_IDX(SetComputeRootUnorderedAccessView), (void *)hk_list_SetComputeRootUnorderedAccessView,
                "CommandList::SetComputeRootUnorderedAccessView");
    }
}

/* The game may talk to a list through any of its interface versions. Hook every
 * distinct vtable the object hands out so no interface slips past us. */
static void hook_command_list_object(void *obj)
{
    static const IID *iids[] =
    {
        &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1, &IID_ID3D12GraphicsCommandList2,
        &IID_ID3D12GraphicsCommandList3, &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5,
        &IID_ID3D12GraphicsCommandList6, &IID_ID3D12GraphicsCommandList7,
    };
    IUnknown *unk = obj;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(iids); i++)
    {
        void *iface = NULL;

        if (SUCCEEDED(unk->lpVtbl->QueryInterface(unk, iids[i], &iface)) && iface)
        {
            hook_command_list_vtable(iface);
            ((IUnknown *)iface)->lpVtbl->Release((IUnknown *)iface);
        }
    }
}

/* ---------------------------------------------------------------- device hooks */

#define DEV_IDX(m) VT_INDEX(ID3D12Device4Vtbl, m)

/* The replacement for CS_ModelSkinning (replace_skinning): sm2fix-skinning.dxil next to
 * the shim, loaded once. Only used when the game hands over the exact original shader. */
static const void *replacement_skinning(size_t *size)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static void *data;
    static size_t len;
    BOOL pending;

    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending)
    {
        WCHAR path[MAX_PATH];
        FILE *f;
        long n;

        path_next_to_self(path, MAX_PATH, L"sm2fix-skinning.dxil");
        if ((f = _wfopen(path, L"rb")))
        {
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 32 && (data = HeapAlloc(GetProcessHeap(), 0, n)) && fread(data, 1, n, f) == (size_t)n
                    && !memcmp(data, "DXBC", 4))
                len = n;
            fclose(f);
        }
        log_msg("replace_skinning: %s sm2fix-skinning.dxil (%zu bytes, hash %016llx)", len ? "loaded" : "could NOT load",
                len, len ? (unsigned long long)sm2_shader_hash(data, len) : 0ull);
        InitOnceComplete(&once, 0, NULL);
    }
    *size = len;
    return len ? data : NULL;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateComputePipelineState(ID3D12Device *device,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pso)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreateComputePipelineState));
    D3D12_COMPUTE_PIPELINE_STATE_DESC swapped;
    const void *alt;
    size_t alt_size;
    HRESULT hr;

    pso_creation_delay();
    if (g_cfg.replace_skinning && desc && desc->CS.pShaderBytecode
            && readable(desc->CS.pShaderBytecode, desc->CS.BytecodeLength)
            && sm2_shader_hash(desc->CS.pShaderBytecode, desc->CS.BytecodeLength) == SM2_SKINNING_HASH
            && (alt = replacement_skinning(&alt_size)))
    {
        swapped = *desc;
        swapped.CS.pShaderBytecode = alt;
        swapped.CS.BytecodeLength = alt_size;
        hr = orig(device, &swapped, riid, pso);
        log_msg("replace_skinning: created the replacement skinning pipeline: hr=%#lx pso=%p", (unsigned long)hr,
                SUCCEEDED(hr) && pso ? *pso : NULL);
        if (FAILED(hr))
            hr = orig(device, desc, riid, pso);   /* fall back to the game's own shader */
    }
    else
        hr = orig(device, desc, riid, pso);

    if (SUCCEEDED(hr) && desc && pso && *pso)
        note_compute_shader(*pso, desc->CS.pShaderBytecode, desc->CS.BytecodeLength, "CreateComputePipelineState");
    else if (FAILED(hr) && desc)
    {
        InterlockedIncrement64(&g_pso_failures);
        log_msg("FAILED CreateComputePipelineState hr=%#lx", (unsigned long)hr);
        dump_shader("cs", desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreatePipelineState(ID3D12Device2 *device,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pso)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreatePipelineState));
    HRESULT hr;
    const void *code = NULL;
    size_t size = 0;

    pso_creation_delay();
    hr = orig(device, desc, riid, pso);

    if (SUCCEEDED(hr) && desc && pso && *pso)
    {
        enum sm2_stream_result r = sm2_stream_find_cs(desc->pPipelineStateSubobjectStream,
                desc->SizeInBytes, &code, &size);

        if (r == SM2_STREAM_COMPUTE)
        {
            note_compute_shader(*pso, code, size, "CreatePipelineState");
            if (g_cfg.replace_skinning && sm2_shader_hash(code, size) == SM2_SKINNING_HASH)
                log_msg("replace_skinning: skinning shader came through CreatePipelineState, NOT replaced");
        }
        else if (r == SM2_STREAM_GRAPHICS && g_cfg.dump_graphics)
            sm2_stream_for_each_shader(desc->pPipelineStateSubobjectStream, desc->SizeInBytes,
                    dump_stream_vertex_shader, NULL);
        else if (r == SM2_STREAM_UNKNOWN || r == SM2_STREAM_MALFORMED)
            log_msg("warning: could not parse pipeline stream (%d) for pso %p", (int)r, *pso);
    }
    else if (FAILED(hr) && desc)
    {
        InterlockedIncrement64(&g_pso_failures);
        log_msg("FAILED CreatePipelineState hr=%#lx", (unsigned long)hr);
        if (sm2_stream_for_each_shader(desc->pPipelineStateSubobjectStream, desc->SizeInBytes,
                dump_stream_shader, NULL) < 0)
            log_msg("  (stream has a subobject type sm2fix can't size; some shaders not dumped)");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateCommandList(ID3D12Device *device, UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial,
        REFIID riid, void **list)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *,
            ID3D12PipelineState *, REFIID, void **) = orig_fn(device, DEV_IDX(CreateCommandList));
    HRESULT hr = orig(device, node_mask, type, allocator, initial, riid, list);

    if (SUCCEEDED(hr) && list && *list)
    {
        hook_command_list_object(*list);
        list_set_type(*list, type);
        list_bind_pso(*list, initial);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateCommandList1(ID3D12Device4 *device, UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **list)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device4 *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS,
            REFIID, void **) = orig_fn(device, DEV_IDX(CreateCommandList1));
    HRESULT hr = orig(device, node_mask, type, flags, riid, list);

    if (SUCCEEDED(hr) && list && *list)
    {
        hook_command_list_object(*list);
        list_set_type(*list, type);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateGraphicsPipelineState(ID3D12Device *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pso)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreateGraphicsPipelineState));
    HRESULT hr;

    pso_creation_delay();
    hr = orig(device, desc, riid, pso);

    if (SUCCEEDED(hr) && desc && g_cfg.dump_graphics && desc->VS.BytecodeLength)
        dump_shader_ex("vs", desc->VS.pShaderBytecode, desc->VS.BytecodeLength, TRUE, 20000);
    if (FAILED(hr) && desc)
    {
        InterlockedIncrement64(&g_pso_failures);
        log_msg("FAILED CreateGraphicsPipelineState hr=%#lx", (unsigned long)hr);
        if (desc->VS.BytecodeLength) dump_shader("vs", desc->VS.pShaderBytecode, desc->VS.BytecodeLength);
        if (desc->HS.BytecodeLength) dump_shader("hs", desc->HS.pShaderBytecode, desc->HS.BytecodeLength);
        if (desc->DS.BytecodeLength) dump_shader("ds", desc->DS.pShaderBytecode, desc->DS.BytecodeLength);
        if (desc->GS.BytecodeLength) dump_shader("gs", desc->GS.pShaderBytecode, desc->GS.BytecodeLength);
        if (desc->PS.BytecodeLength) dump_shader("ps", desc->PS.pShaderBytecode, desc->PS.BytecodeLength);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateRootSignature(ID3D12Device *device, UINT node_mask,
        const void *blob, SIZE_T size, REFIID riid, void **out)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreateRootSignature));
    unsigned int minmax = 0, total = 0;
    uint32_t first = 0;
    void *copy = NULL;
    HRESULT hr;

    InterlockedIncrement64(&g_rootsigs);
    if (blob && size && readable(blob, size) && (copy = HeapAlloc(GetProcessHeap(), 0, size)))
    {
        memcpy(copy, blob, size);
        if (!sm2_rootsig_scan_samplers(copy, size, (enum sm2_minmax_rewrite)g_cfg.minmax, &minmax, &total, &first)
                && minmax)
        {
            LONG64 n = InterlockedAdd64(&g_minmax_static, minmax);

            if (n - minmax < 32)
                log_msg("root signature has %u/%u MIN/MAX static samplers (first filter %#x)%s",
                        minmax, total, first, g_cfg.minmax ? " -> rewritten" : "");
        }
    }
    hr = orig(device, node_mask, (g_cfg.minmax && minmax) ? copy : blob, size, riid, out);
    if (SUCCEEDED(hr) && (g_cfg.probe_skin || g_cfg.capture) && out && copy)
        rootsig_remember(*out, copy, size);
    if (FAILED(hr))
        log_msg("FAILED CreateRootSignature hr=%#lx (%zu bytes)", (unsigned long)hr, (size_t)size);
    if (copy)
        HeapFree(GetProcessHeap(), 0, copy);
    return hr;
}

static void STDMETHODCALLTYPE hk_dev_CreateSampler(ID3D12Device *device, const D3D12_SAMPLER_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_SAMPLER_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)
            = orig_fn(device, DEV_IDX(CreateSampler));
    D3D12_SAMPLER_DESC copy;

    InterlockedIncrement64(&g_samplers_heap);
    if (desc && sm2_filter_is_minmax(desc->Filter))
    {
        LONG64 n = InterlockedIncrement64(&g_minmax_heap);

        if (n <= 16)
            log_msg("heap sampler with MIN/MAX filter %#x%s", (unsigned)desc->Filter, g_cfg.minmax ? " -> rewritten" : "");
        if (g_cfg.minmax)
        {
            copy = *desc;
            copy.Filter = (D3D12_FILTER)sm2_filter_rewrite(desc->Filter, (enum sm2_minmax_rewrite)g_cfg.minmax);
            desc = &copy;
        }
    }
    orig(device, desc, handle);
}

/* Logs what the game asks the GPU layer about and what it is told. If M5 and M1-M4
 * report different answers, the game may pick a different (broken) path. */
static HRESULT STDMETHODCALLTYPE hk_dev_CheckFeatureSupport(ID3D12Device *device, D3D12_FEATURE feature,
        void *data, UINT size)
{
    static volatile LONG seen[128];
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, D3D12_FEATURE, void *, UINT)
            = orig_fn(device, DEV_IDX(CheckFeatureSupport));
    HRESULT hr = orig(device, feature, data, size);
    unsigned int spoof = g_cfg.spoof;
    char what[64];

    if (SUCCEEDED(hr) && data && spoof)
    {
        UINT32 *u = data;
        BOOL changed = FALSE;

        if (feature == D3D12_FEATURE_SHADER_MODEL && (spoof & SM2_SPOOF_SM65) && size >= 4 && u[0] > 0x65)
            u[0] = 0x65, changed = TRUE;
        if (feature == D3D12_FEATURE_D3D12_OPTIONS1 && (spoof & SM2_SPOOF_NO_INT64)
                && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
            ((D3D12_FEATURE_DATA_D3D12_OPTIONS1 *)data)->Int64ShaderOps = FALSE, changed = TRUE;
        if (feature == D3D12_FEATURE_D3D12_OPTIONS4 && (spoof & SM2_SPOOF_NO_FP16)
                && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
            ((D3D12_FEATURE_DATA_D3D12_OPTIONS4 *)data)->Native16BitShaderOpsSupported = FALSE, changed = TRUE;
        if (feature == D3D12_FEATURE_ARCHITECTURE && (spoof & SM2_SPOOF_UMA)
                && size >= sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        {
            ((D3D12_FEATURE_DATA_ARCHITECTURE *)data)->UMA = TRUE;
            ((D3D12_FEATURE_DATA_ARCHITECTURE *)data)->CacheCoherentUMA = TRUE;
            changed = TRUE;
        }
        if (feature == D3D12_FEATURE_ARCHITECTURE1 && (spoof & SM2_SPOOF_UMA)
                && size >= sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        {
            ((D3D12_FEATURE_DATA_ARCHITECTURE1 *)data)->UMA = TRUE;
            ((D3D12_FEATURE_DATA_ARCHITECTURE1 *)data)->CacheCoherentUMA = TRUE;
            changed = TRUE;
        }
        if (feature == D3D12_FEATURE_D3D12_OPTIONS5 && (spoof & SM2_SPOOF_NO_RT)
                && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
            ((D3D12_FEATURE_DATA_D3D12_OPTIONS5 *)data)->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED, changed = TRUE;
        if (changed)
            log_msg("spoofed answer for feature %d", (int)feature);
    }

    if ((unsigned int)feature < ARRAYSIZE(seen) && InterlockedIncrement(&seen[feature]) <= 4)
    {
        snprintf(what, sizeof(what), "feature %d (%u bytes) hr=%#lx", (int)feature, size, (unsigned long)hr);
        log_hex(what, data, size);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateCommandSignature(ID3D12Device *device,
        const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root, REFIID riid, void **out)
{
    static volatile LONG logged;
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_COMMAND_SIGNATURE_DESC *, ID3D12RootSignature *,
            REFIID, void **) = orig_fn(device, DEV_IDX(CreateCommandSignature));
    HRESULT hr = orig(device, desc, root, riid, out);

    if (desc && InterlockedIncrement(&logged) <= 64)
    {
        char types[128] = "";
        UINT i;

        for (i = 0; i < desc->NumArgumentDescs && i < 16; i++)
            snprintf(types + strlen(types), sizeof(types) - strlen(types), "%d ", (int)desc->pArgumentDescs[i].Type);
        log_msg("CreateCommandSignature stride=%u args=[ %s] hr=%#lx", desc->ByteStride, types, (unsigned long)hr);
    }
    return hr;
}

/* ---------------------------------------------------------------- queue hooks */

#define Q_IDX(m) VT_INDEX(ID3D12CommandQueueVtbl, m)

struct queue_state
{
    void *queue;
    D3D12_COMMAND_LIST_TYPE type;
    ID3D12Fence *fence;
    UINT64 value;
};

static struct queue_state g_queues[64];

static struct queue_state *find_queue(void *queue)
{
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(g_queues); i++)
    {
        if (g_queues[i].queue == queue)
            return &g_queues[i];
    }
    return NULL;
}

/* After a palette snapshot: recheck its identity palettes at the next queue submissions. */
static const int32_t *volatile g_palette_bases;
static volatile unsigned int g_palette_nbases;
static volatile LONG g_palette_submits;   /* 0 = idle, else 1 + submissions seen */
static void recheck_palettes_label(const char *when, const int32_t *bases, unsigned int nbases);

static void capture_after_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists);

static void STDMETHODCALLTYPE hk_queue_ExecuteCommandLists(ID3D12CommandQueue *queue, UINT count,
        ID3D12CommandList *const *lists)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *)
            = orig_fn(queue, Q_IDX(ExecuteCommandLists));
    HRESULT (STDMETHODCALLTYPE *signal)(ID3D12CommandQueue *, ID3D12Fence *, UINT64)
            = orig_fn(queue, Q_IDX(Signal));
    struct queue_state *q;
    ID3D12Fence *fence = NULL;
    UINT64 value = 0;

    if (g_palette_submits)
    {
        LONG k = InterlockedIncrement(&g_palette_submits) - 1;

        if (k == 1 || k == 4 || k == 16)
        {
            char when[48];

            snprintf(when, sizeof(when), "at queue submission %ld", k);
            recheck_palettes_label(when, g_palette_bases, g_palette_nbases);
        }
        if (k >= 16)
            g_palette_submits = 0;
    }
    orig(queue, count, lists);
    if (g_cfg.capture)
        capture_after_submit(queue, count, lists);
    if (g_cfg.trace_queries)
        maybe_dump_queries();

    AcquireSRWLockExclusive(&g_lock);
    if ((q = find_queue(queue)))
    {
        InterlockedIncrement64(&g_submits[q->type & 3]);
        if (q->fence)
        {
            fence = q->fence;
            value = ++q->value;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);

    /* MAX mode: don't let the next submission (on any queue) start until this one has
     * finished on the GPU. Removes every cross-queue and CPU/GPU race, at a big FPS cost.
     * The timeout keeps us from deadlocking on work that waits for a later CPU signal. */
    if (g_cfg.mode == SM2_MODE_MAX && fence && signal && SUCCEEDED(signal(queue, fence, value)))
    {
        if (ID3D12Fence_GetCompletedValue(fence) < value)
        {
            HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);

            if (event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, value, event))
                    && WaitForSingleObject(event, 250) == WAIT_TIMEOUT)
            {
                LONG64 n = InterlockedIncrement64(&g_wait_timeouts);

                if (n == 1 || !(n & (n - 1)))
                    log_msg("serialize wait timed out (%lld so far)", (long long)n);
            }
            if (event)
                CloseHandle(event);
        }
        InterlockedIncrement64(&g_waits);
    }
}

static HRESULT STDMETHODCALLTYPE hk_queue_GetTimestampFrequency(ID3D12CommandQueue *queue, UINT64 *freq)
{
    static volatile LONG logged;
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12CommandQueue *, UINT64 *) = orig_fn(queue, Q_IDX(GetTimestampFrequency));
    HRESULT hr = orig ? orig(queue, freq) : E_NOTIMPL;

    if (g_cfg.fake_timestamps && freq)
    {
        if (InterlockedIncrement(&logged) <= 2)
            log_msg("GetTimestampFrequency: D3DMetal says %llu Hz, answering %llu Hz",
                    SUCCEEDED(hr) ? (unsigned long long)*freq : 0ull, (unsigned long long)qpc_freq());
        *freq = qpc_freq();
        g_ts_freq = *freq;
        return S_OK;
    }
    if (SUCCEEDED(hr) && freq)
    {
        g_ts_freq = *freq;
        if (InterlockedIncrement(&logged) <= 4)
            log_msg("GetTimestampFrequency(queue %p) = %llu Hz", (void *)queue, (unsigned long long)*freq);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_queue_GetClockCalibration(ID3D12CommandQueue *queue, UINT64 *gpu, UINT64 *cpu)
{
    static volatile LONG logged;
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12CommandQueue *, UINT64 *, UINT64 *) = orig_fn(queue, Q_IDX(GetClockCalibration));
    HRESULT hr = orig ? orig(queue, gpu, cpu) : E_NOTIMPL;

    if (g_cfg.fake_timestamps)
    {
        UINT64 now = qpc_now();

        if (gpu)
            *gpu = now;
        if (cpu)
            *cpu = now;
        hr = S_OK;
    }
    if (InterlockedIncrement(&logged) <= 4)
        log_msg("GetClockCalibration hr=%#lx gpu=%llu cpu=%llu", (unsigned long)hr,
                gpu ? (unsigned long long)*gpu : 0ull, cpu ? (unsigned long long)*cpu : 0ull);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_queue_Signal(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12CommandQueue *, ID3D12Fence *, UINT64) = orig_fn(queue, Q_IDX(Signal));

    InterlockedIncrement64(&g_fence_signals);
    return orig(queue, fence, value);
}

static HRESULT STDMETHODCALLTYPE hk_queue_Wait(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12CommandQueue *, ID3D12Fence *, UINT64) = orig_fn(queue, Q_IDX(Wait));

    InterlockedIncrement64(&g_fence_waits);
    return orig(queue, fence, value);
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateCommandQueue(ID3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid, void **out)
{
    static const char *types[] = {"direct", "bundle", "compute", "copy"};
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreateCommandQueue));
    HRESULT hr = orig(device, desc, riid, out);
    ID3D12Fence *fence = NULL;
    unsigned int i;

    if (FAILED(hr) || !desc || !out || !*out)
        return hr;

    log_msg("CreateCommandQueue type=%s priority=%d flags=%#x -> %p",
            (unsigned int)desc->Type < 4 ? types[desc->Type] : "?", desc->Priority, desc->Flags, *out);
    hook_slot(*out, Q_IDX(ExecuteCommandLists), (void *)hk_queue_ExecuteCommandLists, "Queue::ExecuteCommandLists");
    hook_slot(*out, Q_IDX(Signal), (void *)hk_queue_Signal, "Queue::Signal");
    hook_slot(*out, Q_IDX(Wait), (void *)hk_queue_Wait, "Queue::Wait");
    if (g_cfg.trace_queries || g_cfg.fake_timestamps)
    {
        hook_slot(*out, Q_IDX(GetTimestampFrequency), (void *)hk_queue_GetTimestampFrequency,
                "Queue::GetTimestampFrequency");
        hook_slot(*out, Q_IDX(GetClockCalibration), (void *)hk_queue_GetClockCalibration, "Queue::GetClockCalibration");
    }

    if (g_cfg.mode == SM2_MODE_MAX)
        device->lpVtbl->CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);

    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < ARRAYSIZE(g_queues); i++)
    {
        if (!g_queues[i].queue)
        {
            g_queues[i].queue = *out;
            g_queues[i].type = desc->Type;
            g_queues[i].fence = fence;
            fence = NULL;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (fence)
        ID3D12Fence_Release(fence);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateQueryHeap(ID3D12Device *device, const D3D12_QUERY_HEAP_DESC *desc,
        REFIID riid, void **out)
{
    static const char *types[] = {"occlusion", "timestamp", "pipeline-stats", "so-stats", "video-decode-stats",
            "copy-queue-timestamp"};
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_QUERY_HEAP_DESC *, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreateQueryHeap));
    HRESULT hr = orig(device, desc, riid, out);

    if (g_cfg.fake_timestamps && SUCCEEDED(hr) && desc && out && *out && desc->Type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP)
        ts_register_heap(*out, desc->Count);
    if (desc)
        log_msg("CreateQueryHeap type=%s count=%u hr=%#lx", (unsigned int)desc->Type < 6 ? types[desc->Type] : "?",
                desc->Count, (unsigned long)hr);
    return hr;
}

/* ------------------------------------------------------- descriptor tracking */

/* With probe_skin, remember what every CBV/SRV/UAV descriptor points at (created or
 * copied), so the views the skinning shader actually reads can be logged: resource,
 * GPU address, FirstElement/NumElements/stride/format, buffer size and heap type. */
struct view_rec
{
    void *res;
    UINT64 va, first, width;
    UINT num, stride, format, flags;
    unsigned short kind;        /* 0 none, 1 SRV, 2 UAV, 3 CBV */
    unsigned short dim;         /* view dimension (1 = buffer) */
    unsigned int heap_type;     /* D3D12_HEAP_TYPE of the resource, 0 if unknown */
};

struct heap_rec
{
    UINT64 cpu, gpu, num;
    UINT inc;
    struct view_rec *recs;
};

#define MAX_TRACKED_HEAPS 64
static struct heap_rec g_heaps[MAX_TRACKED_HEAPS];
static volatile LONG g_heap_count;
static volatile LONG g_view_logs;

static struct view_rec *view_at_cpu(SIZE_T cpu)
{
    LONG i, n = g_heap_count < MAX_TRACKED_HEAPS ? g_heap_count : MAX_TRACKED_HEAPS;

    for (i = 0; i < n; i++)
    {
        struct heap_rec *h = &g_heaps[i];

        if (h->recs && cpu >= h->cpu && cpu < h->cpu + h->num * h->inc && !((cpu - h->cpu) % h->inc))
            return &h->recs[(cpu - h->cpu) / h->inc];
    }
    return NULL;
}

static void fill_resource_info(struct view_rec *r, ID3D12Resource *res)
{
    D3D12_HEAP_PROPERTIES props;
    D3D12_HEAP_FLAGS flags;
    D3D12_RESOURCE_DESC desc;

    r->res = res;
    r->va = r->width = 0;
    r->heap_type = 0;
    if (!res)
        return;
    res->lpVtbl->GetDesc(res, &desc);
    r->width = desc.Width;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        r->va = res->lpVtbl->GetGPUVirtualAddress(res);
    if (SUCCEEDED(res->lpVtbl->GetHeapProperties(res, &props, &flags)))
        r->heap_type = props.Type;
}

static HRESULT STDMETHODCALLTYPE hk_dev_CreateDescriptorHeap(ID3D12Device *device, const D3D12_DESCRIPTOR_HEAP_DESC *desc,
        REFIID riid, void **out)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreateDescriptorHeap));
    HRESULT hr = orig(device, desc, riid, out);

    if (SUCCEEDED(hr) && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && out && *out)
    {
        ID3D12DescriptorHeap *heap = NULL;
        LONG i;

        if (SUCCEEDED(((IUnknown *)*out)->lpVtbl->QueryInterface((IUnknown *)*out, &IID_ID3D12DescriptorHeap,
                (void **)&heap)) && heap)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = {0};
            struct view_rec *recs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)desc->NumDescriptors * sizeof(*recs));

            heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu);
            if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu);
            heap->lpVtbl->Release(heap);
            if (recs && (i = InterlockedIncrement(&g_heap_count) - 1) < MAX_TRACKED_HEAPS)
            {
                g_heaps[i].inc = device->lpVtbl->GetDescriptorHandleIncrementSize(device, desc->Type);
                g_heaps[i].cpu = cpu.ptr;
                g_heaps[i].gpu = gpu.ptr;
                g_heaps[i].num = desc->NumDescriptors;
                g_heaps[i].recs = recs;
                if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                    log_msg("descriptor heap: %u shader-visible CBV/SRV/UAV descriptors, cpu %#llx gpu %#llx, increment %u",
                            desc->NumDescriptors, (unsigned long long)cpu.ptr, (unsigned long long)gpu.ptr, g_heaps[i].inc);
            }
            else if (recs)
                HeapFree(GetProcessHeap(), 0, recs);
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE hk_dev_CreateShaderResourceView(ID3D12Device *device, ID3D12Resource *res,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *,
            D3D12_CPU_DESCRIPTOR_HANDLE) = orig_fn(device, DEV_IDX(CreateShaderResourceView));
    struct view_rec *r;

    orig(device, res, desc, handle);
    if ((r = view_at_cpu(handle.ptr)))
    {
        memset(r, 0, sizeof(*r));
        r->kind = 1;
        fill_resource_info(r, res);
        if (desc)
        {
            r->dim = (unsigned short)desc->ViewDimension;
            r->format = desc->Format;
            if (desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
            {
                r->first = desc->Buffer.FirstElement;
                r->num = desc->Buffer.NumElements;
                r->stride = desc->Buffer.StructureByteStride;
                r->flags = desc->Buffer.Flags;
            }
        }
    }
}

static void STDMETHODCALLTYPE hk_dev_CreateUnorderedAccessView(ID3D12Device *device, ID3D12Resource *res,
        ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12Device *, ID3D12Resource *, ID3D12Resource *,
            const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)
            = orig_fn(device, DEV_IDX(CreateUnorderedAccessView));
    struct view_rec *r;

    orig(device, res, counter, desc, handle);
    if ((r = view_at_cpu(handle.ptr)))
    {
        memset(r, 0, sizeof(*r));
        r->kind = 2;
        fill_resource_info(r, res);
        if (desc)
        {
            r->dim = (unsigned short)desc->ViewDimension;
            r->format = desc->Format;
            if (desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
            {
                r->first = desc->Buffer.FirstElement;
                r->num = desc->Buffer.NumElements;
                r->stride = desc->Buffer.StructureByteStride;
                r->flags = desc->Buffer.Flags;
            }
        }
    }
}

static void STDMETHODCALLTYPE hk_dev_CreateConstantBufferView(ID3D12Device *device,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)
            = orig_fn(device, DEV_IDX(CreateConstantBufferView));
    struct view_rec *r;

    orig(device, desc, handle);
    if ((r = view_at_cpu(handle.ptr)))
    {
        memset(r, 0, sizeof(*r));
        r->kind = 3;
        if (desc)
        {
            r->va = desc->BufferLocation;
            r->num = desc->SizeInBytes;
        }
    }
}

static void copy_view_recs(SIZE_T dst, SIZE_T src, UINT count)
{
    UINT i;

    for (i = 0; i < count; i++)
    {
        struct view_rec *d = view_at_cpu(dst), *s = view_at_cpu(src);

        if (d)
        {
            if (s)
                *d = *s;
            else
                memset(d, 0, sizeof(*d));
        }
        /* all CBV/SRV/UAV heaps share one increment */
        dst += g_heaps[0].inc;
        src += g_heaps[0].inc;
    }
}

static void STDMETHODCALLTYPE hk_dev_CopyDescriptorsSimple(ID3D12Device *device, UINT count,
        D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
            D3D12_DESCRIPTOR_HEAP_TYPE) = orig_fn(device, DEV_IDX(CopyDescriptorsSimple));

    orig(device, count, dst, src, type);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && g_heap_count)
        copy_view_recs(dst.ptr, src.ptr, count);
}

static void STDMETHODCALLTYPE hk_dev_CopyDescriptors(ID3D12Device *device, UINT dst_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes, UINT src_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12Device *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, UINT,
            const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE)
            = orig_fn(device, DEV_IDX(CopyDescriptors));
    UINT di = 0, si = 0, doff = 0, soff = 0;

    orig(device, dst_count, dst_starts, dst_sizes, src_count, src_starts, src_sizes, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !g_heap_count)
        return;
    /* walk both range lists one descriptor at a time (NULL sizes mean 1 each) */
    while (di < dst_count && si < src_count)
    {
        UINT dsize = dst_sizes ? dst_sizes[di] : 1, ssize = src_sizes ? src_sizes[si] : 1;

        if (doff >= dsize) { di++; doff = 0; continue; }
        if (soff >= ssize) { si++; soff = 0; continue; }
        copy_view_recs(dst_starts[di].ptr + (SIZE_T)doff * g_heaps[0].inc,
                src_starts[si].ptr + (SIZE_T)soff * g_heaps[0].inc, 1);
        doff++;
        soff++;
    }
}

static void log_table_views(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    static const char *const kinds[] = { "empty", "SRV", "UAV", "CBV" };
    LONG i, n = g_heap_count < MAX_TRACKED_HEAPS ? g_heap_count : MAX_TRACKED_HEAPS;
    UINT k;

    if (!g_view_window_log && InterlockedIncrement(&g_view_logs) > 12)
        return;
    for (i = 0; i < n; i++)
    {
        struct heap_rec *h = &g_heaps[i];

        if (!h->gpu || base.ptr < h->gpu || base.ptr >= h->gpu + h->num * h->inc)
            continue;
        for (k = 0; k < 6 && (base.ptr - h->gpu) / h->inc + k < h->num; k++)
        {
            const struct view_rec *r = &h->recs[(base.ptr - h->gpu) / h->inc + k];

            log_msg("skin window:   root %u [%u] %s dim=%u res=%p va=%#llx width=%llu heap=%u first=%llu num=%u"
                    " stride=%u format=%u flags=%#x", index, k, kinds[r->kind < 4 ? r->kind : 0], r->dim, r->res,
                    (unsigned long long)r->va, (unsigned long long)r->width, r->heap_type,
                    (unsigned long long)r->first, r->num, r->stride, r->format, r->flags);
            /* the bone palette buffer: structured, 64-byte elements, CPU-visible */
            if (r->kind == 1 && r->dim == 1 && r->stride == 64 && r->heap_type == D3D12_HEAP_TYPE_UPLOAD
                    && r->width >= (1u << 20) && !g_bone_res)
            {
                g_bone_res = r->res;
                g_bone_width = r->width;
                g_bone_first = r->first;
                log_msg("skin window: bone palette buffer is %p (%llu bytes)", r->res, (unsigned long long)r->width);
            }
            /* CPU-visible buffers (upload heap): show the first 32 words the view covers */
            if (r->res && r->dim == 1 && r->heap_type == D3D12_HEAP_TYPE_UPLOAD && (r->kind == 1 || r->kind == 2))
            {
                UINT64 off = r->first * (r->stride ? r->stride : 4);
                D3D12_RANGE none = {0, 0};
                void *map = NULL;

                if (off + 128 <= r->width && SUCCEEDED(((ID3D12Resource *)r->res)->lpVtbl->Map(r->res, 0, &none, &map))
                        && map)
                {
                    const uint32_t *w = (const uint32_t *)((uint8_t *)map + off);
                    char line[32 * 9 + 1];
                    unsigned int j;

                    for (j = 0; j < 32; j++)
                        sprintf(line + j * 9, "%08x ", w[j]);
                    log_msg("skin window:     data: %s", line);
                    ((ID3D12Resource *)r->res)->lpVtbl->Unmap(r->res, 0, &none);
                }
            }
        }
        return;
    }
    log_msg("skin window:   root %u: table not in a tracked heap", index);
}

/* ---------------------------------------------------------------- dispatch capture */

/* With capture = N: for each shader in capture_hashes (the Ziva RT face/cloth shaders), N
 * dispatches are saved to sm2fix-capture\<hash>-<n>.bin: root signature, root arguments,
 * every buffer view the dispatch can see with its contents just before the dispatch, and
 * every UAV's contents just after. gpu-test/replay.exe re-runs the dispatch from that
 * file on another driver, so the Mac's output can be checked against a reference. */
#define CAPTURE_MAX_VIEWS 128
#define CAPTURE_SLOTS 16
#define CAPTURE_VIEW_MAX (8ull << 20)
#define CAPTURE_TOTAL_MAX (48ull << 20)

struct cap_view
{
    uint32_t param, slot, kind, dim, format, num, stride, flags, heap_type, has_after;
    uint64_t first, res_width, byte_off, byte_len, res_id, rb_before, rb_after, va;
    ID3D12Resource *res;
};

struct cap_pending
{
    void *list;
    ID3D12Device *dev;
    ID3D12Resource *rb;
    uint64_t hash, rb_size;
    UINT x, y, z, seq;
    struct root_state root;
    struct sm2_rootsig rs;
    const void *rs_blob;
    SIZE_T rs_size;
    unsigned int nviews;
    struct cap_view v[CAPTURE_MAX_VIEWS];
};

static struct cap_pending *volatile g_cap[CAPTURE_SLOTS];
static volatile LONG g_cap_count[8];
static volatile DWORD g_cap_last[8], g_cap_t0;
static volatile LONG g_cap_written;

static void *real_fn(void *obj, unsigned int index)
{
    void *fn = orig_fn(obj, index);

    return fn ? fn : (*(void ***)obj)[index];
}

static BOOL capture_wanted(uint64_t hash)
{
    DWORD now = GetTickCount();
    unsigned int i;
    LONG n;

    for (i = 0; i < g_cfg.capture_hash_count && i < 8; i++)
        if (g_cfg.capture_hashes[i] == hash)
            break;
    if (i >= g_cfg.capture_hash_count || i >= 8)
        return FALSE;
    if (!g_cap_t0)
    {
        InterlockedCompareExchange((volatile LONG *)&g_cap_t0, (LONG)(now | 1), 0);
        log_msg("capture: first dispatch of a capture shader (%016llx); capturing in %u ms", (unsigned long long)hash,
                g_cfg.capture_after_ms);
    }
    if (now - g_cap_t0 < g_cfg.capture_after_ms)
        return FALSE;
    n = g_cap_count[i];
    if ((unsigned int)n >= g_cfg.capture || (n && now - g_cap_last[i] < 3000))
        return FALSE;
    if (InterlockedCompareExchange(&g_cap_count[i], n + 1, n) != n)
        return FALSE;
    g_cap_last[i] = now;
    return TRUE;
}

static BOOL find_res_by_va(UINT64 va, ID3D12Resource **res, UINT64 *res_va, UINT64 *width)
{
    LONG i, n = g_heap_count < MAX_TRACKED_HEAPS ? g_heap_count : MAX_TRACKED_HEAPS;
    UINT64 k;

    for (i = 0; i < n; i++)
    {
        struct heap_rec *h = &g_heaps[i];

        for (k = 0; h->recs && k < h->num; k++)
        {
            const struct view_rec *r = &h->recs[k];

            if (r->res && r->va && va >= r->va && va < r->va + r->width)
            {
                *res = r->res;
                *res_va = r->va;
                *width = r->width;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void capture_free(struct cap_pending *c)
{
    if (c->rb)
        c->rb->lpVtbl->Release(c->rb);
    if (c->dev)
        c->dev->lpVtbl->Release(c->dev);
    HeapFree(GetProcessHeap(), 0, c);
}

static void capture_drop_list(void *list)
{
    unsigned int i;

    for (i = 0; i < CAPTURE_SLOTS; i++)
    {
        struct cap_pending *c = g_cap[i];

        if (c && c->list == list && InterlockedCompareExchangePointer((void *volatile *)&g_cap[i], NULL, c) == c)
        {
            log_msg("capture: list %p was reset before it ran, dropping its capture of %016llx", list,
                    (unsigned long long)c->hash);
            capture_free(c);
        }
    }
}

static void cap_add_buffer(struct cap_pending *c, struct cap_view *v, UINT64 off, UINT64 len, uint64_t *total)
{
    if (off > v->res_width)
        off = v->res_width;
    if (len > v->res_width - off)
        len = v->res_width - off;
    if (len > CAPTURE_VIEW_MAX || *total + len > CAPTURE_TOTAL_MAX)
        len = 0;
    v->byte_off = off;
    v->byte_len = len;
    *total += (len + 255) & ~255ull;
}

static void capture_dispatch(ID3D12GraphicsCommandList *list, uint64_t hash, UINT x, UINT y, UINT z,
        void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, UINT, UINT))
{
    void (STDMETHODCALLTYPE *copy)(ID3D12GraphicsCommandList *, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64,
            UINT64) = real_fn(list, CL_IDX(CopyBufferRegion));
    static volatile LONG seq;
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_HEAP_PROPERTIES heap;
    D3D12_RESOURCE_DESC desc;
    struct cap_pending *c;
    uint64_t total = 0, off;
    unsigned int i, p, k;
    LONG ri, rn;
    HRESULT hr;

    if (!(c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c))) || !root_get(list, &c->root))
    {
        log_msg("capture: %016llx: no root state for list %p", (unsigned long long)hash, (void *)list);
        goto plain;
    }
    rn = g_rootsig_reg_n < ROOTSIG_MAX ? g_rootsig_reg_n : ROOTSIG_MAX;
    for (ri = 0; ri < rn; ri++)
        if (g_rootsig_reg[ri].obj == c->root.rs)
        {
            c->rs_blob = g_rootsig_reg[ri].blob;
            c->rs_size = g_rootsig_reg[ri].size;
        }
    if (!c->rs_blob || sm2_rootsig_parse(c->rs_blob, c->rs_size, &c->rs))
    {
        log_msg("capture: %016llx: root signature %p unknown or unparsable", (unsigned long long)hash, c->root.rs);
        goto plain;
    }
    c->list = list;
    c->hash = hash;
    c->x = x;
    c->y = y;
    c->z = z;
    for (p = 0; p < c->rs.num_params && p < ROOT_PARAMS; p++)
    {
        UINT64 val = c->root.val[p];

        if (c->rs.param_type[p] == SM2_RS_TABLE && val)
        {
            uint32_t n = sm2_rootsig_table_size(&c->rs, p, 32);
            LONG hi, hn = g_heap_count < MAX_TRACKED_HEAPS ? g_heap_count : MAX_TRACKED_HEAPS;

            for (hi = 0; hi < hn; hi++)
            {
                struct heap_rec *h = &g_heaps[hi];

                if (!h->gpu || val < h->gpu || val >= h->gpu + h->num * h->inc)
                    continue;
                for (k = 0; k < n && (val - h->gpu) / h->inc + k < h->num && c->nviews < CAPTURE_MAX_VIEWS; k++)
                {
                    const struct view_rec *r = &h->recs[(val - h->gpu) / h->inc + k];
                    struct cap_view *v = &c->v[c->nviews++];

                    v->param = p;
                    v->slot = k;
                    v->kind = r->kind;
                    v->dim = r->dim;
                    v->format = r->format;
                    v->num = r->num;
                    v->stride = r->stride;
                    v->flags = r->flags;
                    v->first = r->first;
                    v->heap_type = r->heap_type;
                    v->res_id = (uint64_t)(UINT_PTR)r->res;
                    v->va = r->va;
                    if (r->kind == 3 && r->va)
                    {
                        ID3D12Resource *res;
                        UINT64 res_va, width;

                        if (find_res_by_va(r->va, &res, &res_va, &width))
                        {
                            v->res = res;
                            v->res_width = width;
                            v->res_id = (uint64_t)(UINT_PTR)res;
                            cap_add_buffer(c, v, r->va - res_va, r->num, &total);
                        }
                    }
                    else if ((r->kind == 1 || r->kind == 2) && r->dim == 1 && r->res)
                    {
                        unsigned int es = r->stride ? r->stride : (r->flags & 1) ? 4 : sm2_format_size(r->format);

                        v->res = r->res;
                        v->res_width = r->width;
                        if (es)
                            cap_add_buffer(c, v, r->first * es, (UINT64)r->num * es, &total);
                        v->has_after = r->kind == 2 && v->byte_len;
                        if (v->has_after)
                            total += (v->byte_len + 255) & ~255ull;
                    }
                }
                break;
            }
        }
        else if (c->rs.param_type[p] >= SM2_RS_CBV && val && c->nviews < CAPTURE_MAX_VIEWS)
        {
            struct cap_view *v = &c->v[c->nviews++];
            ID3D12Resource *res;
            UINT64 res_va, width;

            v->param = p;
            v->slot = 0xffffffffu;
            v->kind = c->rs.param_type[p] == SM2_RS_CBV ? 3 : c->rs.param_type[p] == SM2_RS_SRV ? 1 : 2;
            v->va = val;
            if (find_res_by_va(val, &res, &res_va, &width))
            {
                v->res = res;
                v->res_width = width;
                v->res_id = (uint64_t)(UINT_PTR)res;
                cap_add_buffer(c, v, val - res_va, v->kind == 3 ? 65536 : (16u << 20), &total);
                v->has_after = v->kind == 2 && v->byte_len;
                if (v->has_after)
                    total += (v->byte_len + 255) & ~255ull;
            }
        }
    }
    if (!total || FAILED(list->lpVtbl->GetDevice(list, &IID_ID3D12Device, (void **)&c->dev)) || !c->dev)
    {
        log_msg("capture: %016llx: nothing to capture (%u views)", (unsigned long long)hash, c->nviews);
        goto plain;
    }
    memset(&heap, 0, sizeof(heap));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = total;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(hr = c->dev->lpVtbl->CreateCommittedResource(c->dev, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&c->rb)))
    {
        log_msg("capture: %016llx: readback buffer of %llu bytes failed, hr=%#lx", (unsigned long long)hash,
                (unsigned long long)total, (unsigned long)hr);
        c->rb = NULL;
        goto plain;
    }
    c->rb_size = total;

    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->lpVtbl->ResourceBarrier(list, 1, &barrier);
    for (i = 0, off = 0; i < c->nviews; i++)
    {
        struct cap_view *v = &c->v[i];

        if (!v->byte_len)
            continue;
        v->rb_before = off;
        copy(list, c->rb, off, v->res, v->byte_off, v->byte_len);
        off += (v->byte_len + 255) & ~255ull;
    }
    list->lpVtbl->ResourceBarrier(list, 1, &barrier);
    orig(list, x, y, z);
    list->lpVtbl->ResourceBarrier(list, 1, &barrier);
    for (i = 0; i < c->nviews; i++)
    {
        struct cap_view *v = &c->v[i];

        if (!v->has_after)
            continue;
        v->rb_after = off;
        copy(list, c->rb, off, v->res, v->byte_off, v->byte_len);
        off += (v->byte_len + 255) & ~255ull;
    }
    c->seq = (UINT)InterlockedIncrement(&seq);
    for (i = 0; i < CAPTURE_SLOTS; i++)
        if (!InterlockedCompareExchangePointer((void *volatile *)&g_cap[i], c, NULL))
            break;
    if (i == CAPTURE_SLOTS)
    {
        capture_free(c);
        return;
    }
    log_msg("capture: recorded %016llx Dispatch(%u, %u, %u) on list %p: %u views, %llu bytes", (unsigned long long)hash,
            x, y, z, (void *)list, c->nviews, (unsigned long long)total);
    return;

plain:
    orig(list, x, y, z);
    if (c)
        capture_free(c);
}

static void capture_write(struct cap_pending *c)
{
    static const char magic[4] = { 'S', 'M', '2', 'C' };
    D3D12_RANGE range = {0, (SIZE_T)c->rb_size}, none = {0, 0};
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    const uint8_t *map = NULL;
    uint32_t u, i;
    FILE *f;

    path_next_to_self(dir, MAX_PATH, L"sm2fix-capture");
    CreateDirectoryW(dir, NULL);
    swprintf(path, MAX_PATH, L"%ls\\%016llx-%u.bin", dir, (unsigned long long)c->hash, c->seq);
    if (FAILED(c->rb->lpVtbl->Map(c->rb, 0, &range, (void **)&map)) || !map || !(f = _wfopen(path, L"wb")))
    {
        log_msg("capture: couldn't save %016llx", (unsigned long long)c->hash);
        if (map)
            c->rb->lpVtbl->Unmap(c->rb, 0, &none);
        return;
    }
#define W32(v) do { u = (uint32_t)(v); fwrite(&u, 4, 1, f); } while (0)
#define W64(v) do { uint64_t q_ = (uint64_t)(v); fwrite(&q_, 8, 1, f); } while (0)
    fwrite(magic, 4, 1, f);
    W32(1);
    W64(c->hash);
    W32(c->x); W32(c->y); W32(c->z);
    W32(c->rs_size);
    fwrite(c->rs_blob, 1, c->rs_size, f);
    W32(ROOT_PARAMS);
    for (i = 0; i < ROOT_PARAMS; i++)
    {
        W64(c->root.val[i]);
        fwrite(c->root.consts[i], 4, 16, f);
    }
    W32(c->nviews);
    for (i = 0; i < c->nviews; i++)
    {
        const struct cap_view *v = &c->v[i];

        W32(v->param); W32(v->slot); W32(v->kind); W32(v->dim); W32(v->format); W32(v->num); W32(v->stride);
        W32(v->flags); W32(v->heap_type); W32(v->has_after);
        W64(v->first); W64(v->res_width); W64(v->byte_off); W64(v->byte_len); W64(v->res_id); W64(v->va);
    }
    for (i = 0; i < c->nviews; i++)
        if (c->v[i].byte_len)
            fwrite(map + c->v[i].rb_before, 1, c->v[i].byte_len, f);
    for (i = 0; i < c->nviews; i++)
        if (c->v[i].has_after)
            fwrite(map + c->v[i].rb_after, 1, c->v[i].byte_len, f);
#undef W32
#undef W64
    fclose(f);
    c->rb->lpVtbl->Unmap(c->rb, 0, &none);
    log_msg("capture: saved sm2fix-capture\\%016llx-%u.bin (%u views)", (unsigned long long)c->hash, c->seq, c->nviews);
    InterlockedIncrement(&g_cap_written);
}

static void capture_after_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    unsigned int i, j;

    for (i = 0; i < CAPTURE_SLOTS; i++)
    {
        struct cap_pending *c = g_cap[i];
        ID3D12Fence *fence = NULL;
        HANDLE event;
        BOOL mine = FALSE;

        if (!c)
            continue;
        for (j = 0; j < count; j++)
        {
            void *gl = NULL;

            /* the submitted interface may differ from the one recorded on; compare objects */
            if (lists[j] == c->list)
                mine = TRUE;
            else if (lists[j] && SUCCEEDED(((IUnknown *)lists[j])->lpVtbl->QueryInterface((IUnknown *)lists[j],
                    &IID_ID3D12GraphicsCommandList, &gl)) && gl)
            {
                mine = gl == c->list;
                ((IUnknown *)gl)->lpVtbl->Release((IUnknown *)gl);
            }
            if (mine)
                break;
        }
        if (!mine || InterlockedCompareExchangePointer((void *volatile *)&g_cap[i], NULL, c) != c)
            continue;
        if (SUCCEEDED(c->dev->lpVtbl->CreateFence(c->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence))
                && fence)
        {
            HRESULT (STDMETHODCALLTYPE *signal)(ID3D12CommandQueue *, ID3D12Fence *, UINT64)
                    = real_fn(queue, Q_IDX(Signal));

            event = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (event && SUCCEEDED(signal(queue, fence, 1)) && SUCCEEDED(fence->lpVtbl->SetEventOnCompletion(fence, 1, event))
                    && WaitForSingleObject(event, 10000) == WAIT_OBJECT_0)
                capture_write(c);
            else
                log_msg("capture: GPU didn't finish the captured %016llx dispatch in 10 s", (unsigned long long)c->hash);
            if (event)
                CloseHandle(event);
            fence->lpVtbl->Release(fence);
        }
        capture_free(c);
    }
}

static void hook_device_object(void *obj)
{
    static const struct
    {
        const IID *iid;
        unsigned int version;
    }
    iids[] =
    {
        {&IID_ID3D12Device, 0}, {&IID_ID3D12Device1, 1}, {&IID_ID3D12Device2, 2}, {&IID_ID3D12Device3, 3},
        {&IID_ID3D12Device4, 4}, {&IID_ID3D12Device5, 5}, {&IID_ID3D12Device6, 6}, {&IID_ID3D12Device7, 7},
        {&IID_ID3D12Device8, 8}, {&IID_ID3D12Device9, 9}, {&IID_ID3D12Device10, 10},
    };
    IUnknown *unk = obj;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(iids); i++)
    {
        void *dev = NULL;

        if (FAILED(unk->lpVtbl->QueryInterface(unk, iids[i].iid, &dev)) || !dev)
            continue;
        hook_slot(dev, DEV_IDX(CreateComputePipelineState), (void *)hk_dev_CreateComputePipelineState,
                "Device::CreateComputePipelineState");
        hook_slot(dev, DEV_IDX(CreateCommandList), (void *)hk_dev_CreateCommandList, "Device::CreateCommandList");
        hook_slot(dev, DEV_IDX(CreateGraphicsPipelineState), (void *)hk_dev_CreateGraphicsPipelineState,
                "Device::CreateGraphicsPipelineState");
        hook_slot(dev, DEV_IDX(CreateRootSignature), (void *)hk_dev_CreateRootSignature, "Device::CreateRootSignature");
        hook_slot(dev, DEV_IDX(CheckFeatureSupport), (void *)hk_dev_CheckFeatureSupport, "Device::CheckFeatureSupport");
        hook_slot(dev, DEV_IDX(CreateSampler), (void *)hk_dev_CreateSampler, "Device::CreateSampler");
        if (g_cfg.trace_queries || g_cfg.fake_timestamps)
            hook_slot(dev, DEV_IDX(CreateQueryHeap), (void *)hk_dev_CreateQueryHeap, "Device::CreateQueryHeap");
        hook_slot(dev, DEV_IDX(CreateCommandSignature), (void *)hk_dev_CreateCommandSignature,
                "Device::CreateCommandSignature");
        hook_slot(dev, DEV_IDX(CreateCommandQueue), (void *)hk_dev_CreateCommandQueue, "Device::CreateCommandQueue");
        if (g_cfg.probe_skin || g_cfg.capture)
        {
            hook_slot(dev, DEV_IDX(CreateDescriptorHeap), (void *)hk_dev_CreateDescriptorHeap, "Device::CreateDescriptorHeap");
            hook_slot(dev, DEV_IDX(CreateShaderResourceView), (void *)hk_dev_CreateShaderResourceView,
                    "Device::CreateShaderResourceView");
            hook_slot(dev, DEV_IDX(CreateUnorderedAccessView), (void *)hk_dev_CreateUnorderedAccessView,
                    "Device::CreateUnorderedAccessView");
            hook_slot(dev, DEV_IDX(CreateConstantBufferView), (void *)hk_dev_CreateConstantBufferView,
                    "Device::CreateConstantBufferView");
            hook_slot(dev, DEV_IDX(CopyDescriptorsSimple), (void *)hk_dev_CopyDescriptorsSimple,
                    "Device::CopyDescriptorsSimple");
            hook_slot(dev, DEV_IDX(CopyDescriptors), (void *)hk_dev_CopyDescriptors, "Device::CopyDescriptors");
        }
        if (iids[i].version >= 2)
            hook_slot(dev, DEV_IDX(CreatePipelineState), (void *)hk_dev_CreatePipelineState,
                    "Device::CreatePipelineState");
        if (iids[i].version >= 4)
            hook_slot(dev, DEV_IDX(CreateCommandList1), (void *)hk_dev_CreateCommandList1,
                    "Device::CreateCommandList1");
        ((IUnknown *)dev)->lpVtbl->Release((IUnknown *)dev);
    }
}

/* ------------------------------------------------------- real d3d12 + exports */

static HRESULT (WINAPI *p_D3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static HRESULT (WINAPI *p_D3D12GetDebugInterface)(REFIID, void **);
static HRESULT (WINAPI *p_D3D12SerializeRootSignature)(const void *, int, void **, void **);
static HRESULT (WINAPI *p_D3D12SerializeVersionedRootSignature)(const void *, void **, void **);
static HRESULT (WINAPI *p_D3D12CreateRootSignatureDeserializer)(const void *, SIZE_T, REFIID, void **);
static HRESULT (WINAPI *p_D3D12CreateVersionedRootSignatureDeserializer)(const void *, SIZE_T, REFIID, void **);
static HRESULT (WINAPI *p_D3D12EnableExperimentalFeatures)(UINT, const IID *, void *, UINT *);
static HRESULT (WINAPI *p_D3D12GetInterface)(REFCLSID, REFIID, void **);

static BOOL CALLBACK init_once(INIT_ONCE *once, void *param, void **context)
{
    static const char *mode_names[] = {"off", "hash", "all", "max"};
    WCHAR path[MAX_PATH];
    unsigned int i;

    log_msg("sm2fix loaded: mode=%s log_shaders=%d hashes=%u spoof=%#x dump_compute=%d dump_graphics=%d minmax=%d",
            mode_names[g_cfg.mode], g_cfg.log_shaders, g_cfg.hash_count, g_cfg.spoof, g_cfg.dump_compute,
            g_cfg.dump_graphics, g_cfg.minmax);
    log_msg("profile=%d trace_queries=%d fake_timestamps=%d", g_cfg.profile, g_cfg.trace_queries, g_cfg.fake_timestamps);
    for (i = 0; i < g_cfg.hash_count; i++)
        log_msg("  target hash %016llx", (unsigned long long)g_cfg.hashes[i]);

    if (g_cfg.real_d3d12[0])
    {
        g_real = LoadLibraryA(g_cfg.real_d3d12);
        log_msg("loading real d3d12 from config path %s -> %p", g_cfg.real_d3d12, g_real);
    }
    else
    {
        UINT len = GetSystemDirectoryW(path, MAX_PATH);

        if (len && len < MAX_PATH - 12)
        {
            wcscat(path, L"\\d3d12.dll");
            g_real = LoadLibraryW(path);
            log_msg("loading real d3d12 from %ls -> %p", path, g_real);
        }
    }

    if (g_real == g_self)
    {
        log_msg("ERROR: the 'real' d3d12.dll resolved to this proxy. Don't install sm2fix into system32;"
                " put it next to Spider-Man2.exe (or set real_d3d12= in sm2fix.ini).");
        g_real = NULL;
    }
    if (!g_real)
    {
        log_msg("ERROR: could not load the real d3d12.dll (error %lu)", GetLastError());
        return TRUE;
    }

#define RESOLVE(name) p_##name = (void *)GetProcAddress(g_real, #name)
    RESOLVE(D3D12CreateDevice);
    RESOLVE(D3D12GetDebugInterface);
    RESOLVE(D3D12SerializeRootSignature);
    RESOLVE(D3D12SerializeVersionedRootSignature);
    RESOLVE(D3D12CreateRootSignatureDeserializer);
    RESOLVE(D3D12CreateVersionedRootSignatureDeserializer);
    RESOLVE(D3D12EnableExperimentalFeatures);
    RESOLVE(D3D12GetInterface);
#undef RESOLVE
    return TRUE;
}

static void ensure_init(void)
{
    InitOnceExecuteOnce(&g_init_once, init_once, NULL, NULL);
}

HRESULT WINAPI sm2_D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **device)
{
    HRESULT hr;

    ensure_init();
    if (!p_D3D12CreateDevice)
        return E_FAIL;
    hr = p_D3D12CreateDevice(adapter, level, riid, device);
    if (SUCCEEDED(hr) && device && *device)
    {
        log_msg("D3D12CreateDevice ok -> %p", *device);
        hook_device_object(*device);
    }
    return hr;
}

HRESULT WINAPI sm2_D3D12GetDebugInterface(REFIID riid, void **debug)
{
    ensure_init();
    return p_D3D12GetDebugInterface ? p_D3D12GetDebugInterface(riid, debug) : E_NOINTERFACE;
}

HRESULT WINAPI sm2_D3D12SerializeRootSignature(const void *desc, int version, void **blob, void **error)
{
    ensure_init();
    return p_D3D12SerializeRootSignature ? p_D3D12SerializeRootSignature(desc, version, blob, error) : E_NOTIMPL;
}

HRESULT WINAPI sm2_D3D12SerializeVersionedRootSignature(const void *desc, void **blob, void **error)
{
    ensure_init();
    return p_D3D12SerializeVersionedRootSignature
            ? p_D3D12SerializeVersionedRootSignature(desc, blob, error) : E_NOTIMPL;
}

HRESULT WINAPI sm2_D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out)
{
    ensure_init();
    return p_D3D12CreateRootSignatureDeserializer
            ? p_D3D12CreateRootSignatureDeserializer(data, size, riid, out) : E_NOTIMPL;
}

HRESULT WINAPI sm2_D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T size,
        REFIID riid, void **out)
{
    ensure_init();
    return p_D3D12CreateVersionedRootSignatureDeserializer
            ? p_D3D12CreateVersionedRootSignatureDeserializer(data, size, riid, out) : E_NOTIMPL;
}

HRESULT WINAPI sm2_D3D12EnableExperimentalFeatures(UINT count, const IID *iids, void *structs, UINT *sizes)
{
    ensure_init();
    return p_D3D12EnableExperimentalFeatures
            ? p_D3D12EnableExperimentalFeatures(count, iids, structs, sizes) : E_NOINTERFACE;
}

HRESULT WINAPI sm2_D3D12GetInterface(REFCLSID clsid, REFIID riid, void **out)
{
    ensure_init();
    log_msg("D3D12GetInterface called: devices created through ID3D12DeviceFactory are not hooked");
    return p_D3D12GetInterface ? p_D3D12GetInterface(clsid, riid, out) : E_NOINTERFACE;
}

/* ------------------------------------------------------------ skin probe */

/* Static analysis of Spider-Man2.exe (build with launcher bytes below) found the
 * per-frame CPU skin-job launcher at RVA 0x2b42560 (called once per frame with the
 * skinning manager, a global at RVA 0xbf60300). It launches skin jobs only if
 *   - the job-system flag at RVA 0xc2ca194 is 0, and
 *   - the request count at manager+0x30 is non-zero.
 * This probe redirects the launcher's first 15 bytes (three register saves) to log
 * those values, then runs the original code. It only patches if the bytes match. */
#define SKIN_MANAGER_RVA  (g_cfg.manager_rva)
static const uint8_t skin_launcher_bytes[15] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
};

static void (*g_skin_trampoline)(void *, void *, void *, void *);
static volatile LONG64 g_skin_calls, g_skin_with_requests, g_skin_flag_blocked;
static volatile LONG g_skin_last_tick, g_skin_logged;
static uint8_t *g_game_base;

static void log_skin_state(const uint8_t *mgr, const char *why)
{
    const uint32_t *m = (const uint32_t *)mgr;
    const uint32_t *js = (const uint32_t *)(g_game_base + g_cfg.probe_jobsys_rva);

    if (!readable(mgr, 0x80))
        return;
    if (!readable(js, 0x40))
        js = (const uint32_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
    log_msg("skin launcher (%s): calls=%lld with-requests=%lld blocked-by-flag=%lld | mgr=%p +08=%u +0c=%u +10=%u"
            " +14=%u +30(requests)=%d +38=%u +3c=%u +48=%u +50=%u +54=%u +78(frames)=%u | jobsys flag(+24)=%u"
            " jobsys: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
            why, (long long)g_skin_calls, (long long)g_skin_with_requests, (long long)g_skin_flag_blocked,
            (const void *)mgr, m[2], m[3], m[4], m[5], (int)m[12], m[14], m[15], m[18], m[20], m[21], m[30],
            js[9], js[0], js[1], js[2], js[3], js[4], js[5], js[6], js[7], js[8], js[10]);
}

static void hk_skin_launcher(void *mgr, void *a2, void *a3, void *a4)
{
    const uint8_t *m = mgr;
    LONG now, last;

    InterlockedIncrement64(&g_skin_calls);
    if (m && readable(m, 0x80))
    {
        if (*(const int32_t *)(m + 0x30) > 0)
            InterlockedIncrement64(&g_skin_with_requests);
        if (readable(g_game_base + g_cfg.probe_jobsys_rva + 0x24, 4)
                && *(const uint32_t *)(g_game_base + g_cfg.probe_jobsys_rva + 0x24))
            InterlockedIncrement64(&g_skin_flag_blocked);
        if (InterlockedIncrement(&g_skin_logged) <= 5)
            log_skin_state(m, "first calls");
        now = (LONG)GetTickCount();
        last = g_skin_last_tick;
        if (now - last > 3000 && InterlockedCompareExchange(&g_skin_last_tick, now, last) == last)
            log_skin_state(m, "periodic");
    }
    g_skin_trampoline(mgr, a2, a3, a4);
}

/* Redirects the first n (>= 12) bytes at target to hook. Returns a trampoline that runs
 * those bytes and continues at target+n, or NULL. The caller checks the bytes first:
 * they must be position-independent instructions ending exactly at target+n. */
static void *patch_prologue(uint8_t *target, size_t n, const void *hook)
{
    uint8_t *tramp, patch[32];
    DWORD old;

    if (n < 12 || n > sizeof(patch))
        return NULL;
    if (!(tramp = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        return NULL;
    /* trampoline: original n bytes, then mov rax, target+n; jmp rax */
    memcpy(tramp, target, n);
    tramp[n] = 0x48; tramp[n + 1] = 0xb8;
    *(uint64_t *)(tramp + n + 2) = (uint64_t)(target + n);
    tramp[n + 10] = 0xff; tramp[n + 11] = 0xe0;
    FlushInstructionCache(GetCurrentProcess(), tramp, n + 12);

    if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old))
        return NULL;
    /* mov rax, hook; jmp rax; nops (rax is free at function entry) */
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0x48; patch[1] = 0xb8;
    *(uint64_t *)(patch + 2) = (uint64_t)hook;
    patch[10] = 0xff; patch[11] = 0xe0;
    memcpy(target, patch, n);
    VirtualProtect(target, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, n);
    return tramp;
}

static void install_skin_probe(void)
{
    uint8_t *target = g_game_base + g_cfg.probe_skin_rva;

    if (!readable(target, 15) || memcmp(target, skin_launcher_bytes, 15))
    {
        log_msg("probe_skin: launcher bytes don't match this game build, not patching");
        return;
    }
    if (!(g_skin_trampoline = patch_prologue(target, 15, hk_skin_launcher)))
        return;
    log_msg("probe_skin: hooked skin launcher at %p (manager global at %p)", (void *)target,
            (void *)(g_game_base + SKIN_MANAGER_RVA));
}

/* ------------------------------------------------------------ SkinningSB budget */

/* CS_ModelSkinning work is written into the "SkinningSB" structured buffer, one 32-byte
 * entry per 1280 vertices, and dispatched as Dispatch(min(entries, capacity), 1, 1)
 * (function at RVA 0x2b08f00). The capacity is RenderBudgets.ModelSkinGroupMax ("the
 * maximum number of gpu thread group elements ModelSkinningGroupCB in the skinning
 * structured buffer"), read once by this stub at RVA 0x2b087c0:
 *     44 8b 05 d1      mov  r8d, [ModelSkinGroupMax]
 *     48 8d 15 d2      lea  rdx, "SkinningSB"
 *     41 b9 20 00 00 00 mov r9d, 32               ; stride
 *     48 8d 0d d3      lea  rcx, [SkinningSB object] ; +0x10 = capacity, +0x14 = stride
 *     e9 rel           jmp  create_structured_buffer
 * Work past the capacity is silently dropped, and those meshes keep their bind pose.
 * The hook replaces the stub: it logs the budgets, optionally raises ModelSkinGroupMax,
 * then calls the game's create function itself. Addresses come from the stub's own
 * displacements, so only the opcodes have to match. */
struct skin_sb
{
    int32_t *budget;            /* &RenderBudgets.ModelSkinGroupMax (struct starts 0xc before) */
    const char *name;
    uint32_t *object;
    uint64_t (*create)(void *object, const char *name, uint32_t count, uint32_t stride);
    int32_t *count;             /* skin entries queued this frame (read by the dispatch function) */
};
static struct skin_sb g_sb;
static void (*g_dispatch_trampoline)(void *, void *, void *, void *);
static volatile LONG64 g_sb_dispatch_calls, g_sb_dispatch_work, g_sb_dispatch_starved;
static volatile LONG g_sb_last_tick, g_sb_logged;

static const char *const render_budget_names[28] = {
    "ModelInstMax", "ModelSkinInstMax", "ModelSkinSubsetMax", "ModelSkinGroupMax",
    "GpuRegistryStaticObjMax", "GpuRegistryHibernateObjMax", "GpuRegistryPerFrameObjMax",
    "LightMax", "VolumeMax", "DynamicDecalMax", "StaticDecalMax", "CurveMax", "VisualEffectInstMax",
    "ImpostorMax", "TerrainMax", "LandscapeMax", "ComponentMax", "ComponentEntryMax",
    "ComponentBlockSizeMax", "ActorMax", "ActorGroupMax", "ScriptVarMax", "SignalRelayMax",
    "DeferredEventHandlerMax", "ModelRenderInfoRatio", "ModelCompositeMatRatio",
    "ModelTriangleSortJobsMax", "IncludeCurvesInSceneDatabase",
};

static void log_render_budgets(const char *why)
{
    const int32_t *rb = g_sb.budget - 3;
    char line[1024];
    size_t len = 0;
    unsigned int i;

    if (!readable(rb, sizeof(int32_t) * 28))
        return;
    for (i = 0; i < 28 && len < sizeof(line) - 64; i++)
        len += snprintf(line + len, sizeof(line) - len, " %s=%d", render_budget_names[i], (int)rb[i]);
    log_msg("RenderBudgets (%s):%s", why, line);
}

static uint64_t hk_skin_sb_create(void)
{
    int32_t before = *g_sb.budget;
    uint64_t ret;

    log_render_budgets("SkinningSB about to be created");
    if (g_cfg.skin_budget && (before < 0 || (uint32_t)before < g_cfg.skin_budget))
    {
        *g_sb.budget = (int32_t)g_cfg.skin_budget;
        log_msg("skin_budget: raising ModelSkinGroupMax %d -> %u", (int)before, g_cfg.skin_budget);
    }
    ret = g_sb.create(g_sb.object, g_sb.name, (uint32_t)*g_sb.budget, 32);
    log_msg("SkinningSB created: ok=%u capacity=%u entries, stride=%u (ModelSkinGroupMax was %d)",
            (unsigned)(ret & 0xff), g_sb.object[4], g_sb.object[5], (int)before);
    return ret;
}

static void log_skin_dispatch(const char *why)
{
    log_msg("skin dispatch (%s): calls=%lld with-work=%lld over-capacity=%lld | entries queued=%d"
            " SkinningSB capacity=%u ModelSkinGroupMax=%d | D3D12 seen inside: SetPipelineState=%lld (skinning=%lld,"
            " last %016llx) Dispatch=%lld (skinning=%lld, last x=%u) ExecuteIndirect=%lld",
            why, (long long)g_sb_dispatch_calls, (long long)g_sb_dispatch_work, (long long)g_sb_dispatch_starved,
            g_sb.count ? (int)*g_sb.count : -1, g_sb.object ? g_sb.object[4] : 0,
            g_sb.budget ? (int)*g_sb.budget : -1, (long long)g_win_set_pso, (long long)g_win_set_pso_skin,
            (unsigned long long)g_win_last_hash, (long long)g_win_dispatch, (long long)g_win_dispatch_skin,
            g_win_last_x, (long long)g_win_indirect);
}

/* The dispatch function talks to an engine render context, r14[index]:
 *   +0x406: 4c 8d 35 disp   lea r14, [contexts]
 *   +0x428: 0f b6 05 disp   movzx eax, byte [current context index]
 * then calls vtable+0x148 (dispatch), after +0x220/+0x228 earlier. Log those methods as
 * game RVAs so they can be disassembled. */
static void log_skin_context(void)
{
    const uint8_t *d = g_game_base + g_cfg.probe_dispatch_rva;
    void **contexts, **ctx, **vtbl;
    uint8_t index;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_game_base + ((IMAGE_DOS_HEADER *)g_game_base)->e_lfanew);
    uintptr_t lo = (uintptr_t)g_game_base, hi = lo + nt->OptionalHeader.SizeOfImage;

    if (!readable(d + 0x406, 0x30) || memcmp(d + 0x406, "\x4c\x8d\x35", 3) || memcmp(d + 0x428, "\x0f\xb6\x05", 3))
    {
        log_msg("skin context: dispatch function layout differs, not decoding");
        return;
    }
    contexts = (void **)(d + 0x40d + *(const int32_t *)(d + 0x409));
    index = *(const uint8_t *)(d + 0x42f + *(const int32_t *)(d + 0x42b));
    if (!readable(contexts + index, 8) || !(ctx = contexts[index]) || !readable(ctx, 8)
            || !(vtbl = *ctx) || !readable(vtbl, 0x240))
    {
        log_msg("skin context: contexts=%p index=%u unreadable", (void *)contexts, index);
        return;
    }
#define SKIN_RVA(p) ((uintptr_t)(p) >= lo && (uintptr_t)(p) < hi ? (unsigned long long)((uintptr_t)(p) - lo) : 0ull)
    log_msg("skin context: contexts=%p index=%u ctx=%p vtbl rva=%#llx | +0x148 (dispatch) rva=%#llx"
            " +0x220 rva=%#llx +0x228 (set shader) rva=%#llx",
            (void *)contexts, index, (void *)ctx, SKIN_RVA(vtbl), SKIN_RVA(vtbl[0x148 / 8]),
            SKIN_RVA(vtbl[0x220 / 8]), SKIN_RVA(vtbl[0x228 / 8]));
#undef SKIN_RVA

    /* +0x1d9: 8b 1d disp (mov ebx, [CS_ModelSkinning index]); +0x1e1: e8 rel (call getter);
     * getter+0xd: 48 8b 05 disp (mov rax, [shader table]); entries are 40 bytes, the
     * first qword is the engine's handle (-1 = not created yet). */
    if (readable(d + 0x1d9, 0x10) && !memcmp(d + 0x1d9, "\x8b\x1d", 2) && d[0x1e1] == 0xe8)
    {
        const int32_t *shader_index = (const int32_t *)(d + 0x1df + *(const int32_t *)(d + 0x1db));
        const uint8_t *getter = d + 0x1e6 + *(const int32_t *)(d + 0x1e2);
        const uint64_t *entry;
        uint8_t *const *table;

        if (readable(shader_index, 4) && readable(getter, 0x14) && !memcmp(getter + 0xd, "\x48\x8b\x05", 3)
                && readable((table = (uint8_t *const *)(getter + 0x14 + *(const int32_t *)(getter + 0x10))), 8)
                && *table && readable((entry = (const uint64_t *)(*table + 40 * (size_t)*shader_index)), 40))
            log_msg("skin shader slot: index=%d entry %016llx %016llx %016llx %016llx %016llx",
                    (int)*shader_index, (unsigned long long)entry[0], (unsigned long long)entry[1],
                    (unsigned long long)entry[2], (unsigned long long)entry[3], (unsigned long long)entry[4]);
        else
            log_msg("skin shader slot: could not decode the shader table");
    }
}

/* The dispatch function's work queue (from its own instructions, see install_skin_budget):
 * 24-byte items, item+8 = bone palette base (in 64-byte bones, -1 = skipped), item+0x10 =
 * the mesh subset record. */
static const uint8_t *g_queue_base_ptr;     /* &queue array pointer */
static const int32_t *g_queue_sel, *g_queue_alt;

struct palette_stats
{
    unsigned int items, skipped, identity, zero, other, out_of_range;
    unsigned int flag_identity[8], flag_animated[8];    /* by bit of the record's flag byte (+4) */
    unsigned int noflag_identity, noflag_animated;      /* record unreadable */
    unsigned int remap_identity, remap_animated;        /* record +0x1c != -1: bones looked up via a remap table */
    /* per value of a mesh field (low byte): identity / animated counts */
    unsigned int mflags_id[256], mflags_an[256];        /* mesh +0x14 */
    unsigned int m1c_id[256], m1c_an[256];              /* mesh +0x1c */
    unsigned int m34_id[256], m34_an[256];              /* mesh +0x34 */
};

static void log_histogram(const char *name, const unsigned int *id, const unsigned int *an)
{
    char line[1024];
    size_t len = 0;
    int v;

    for (v = 0; v < 256 && len < sizeof(line) - 40; v++)
        if (id[v] || an[v])
            len += snprintf(line + len, sizeof(line) - len, " %#x:%u/%u", v, id[v], an[v]);
    log_msg("skin palettes: by %s (value:identity/animated):%s", name, len ? line : " none");
}

static void log_record(const char *what, int32_t item, int32_t base, const uint8_t *rec)
{
    char line[48 * 3 + 1], mesh[64 * 3 + 1];
    const uint8_t *m;
    int k;

    if (!rec || !readable(rec, 48))
        return;
    for (k = 0; k < 48; k++)
        sprintf(line + k * 3, "%02x ", rec[k]);
    m = *(const uint8_t *const *)(rec + 8);
    mesh[0] = 0;
    if (m && readable(m, 64))
        for (k = 0; k < 64; k++)
            sprintf(mesh + k * 3, "%02x ", m[k]);
    log_msg("skin record %s: item %d palette %d rec %p: %s| mesh %p: %s", what, item, base, (const void *)rec, line,
            (const void *)m, mesh);
}

static int bone_is_identity(const float *m)
{
    static const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int k;

    for (k = 0; k < 16; k++)
        if (k != 15 && fabsf(m[k] - id[k]) > 1e-4f)
            return 0;
    return 1;
}

static int bone_is_zero(const float *m)
{
    int k;

    for (k = 0; k < 12; k++)
        if (m[k] != 0.0f)
            return 0;
    return 1;
}

/* Classifies the first 4 bones of every queued palette: all identity (no animation
 * applied: the mesh would render in its bind pose), all zero, or animated. */
static void analyse_palettes(const char *when, int32_t n, int32_t *bases, unsigned int *nbases, int sample)
{
    struct palette_stats st = {0};
    ID3D12Resource *res = g_bone_res;
    D3D12_RANGE none = {0, 0};
    const uint8_t *queue, *bones = NULL;
    int32_t i, sel;
    unsigned int shown_id = 0, shown_anim = 0;

    if (!res || !g_queue_base_ptr || n <= 0 || !readable(g_queue_base_ptr, 8))
        return;
    sel = *g_queue_sel ? *g_queue_alt : 0;
    queue = *(const uint8_t *const *)g_queue_base_ptr + (size_t)sel * 24;
    if (!readable(queue, (size_t)n * 24))
        return;
    if (FAILED(res->lpVtbl->Map(res, 0, &none, (void **)&bones)) || !bones)
        return;
    bones += g_bone_first * 64;
    for (i = 0; i < n && i < 30000; i++)
    {
        int32_t base = *(const int32_t *)(queue + (size_t)i * 24 + 8);
        const float *m;
        int k, ident = 1, zero = 1;

        st.items++;
        if (base < 0)
        {
            st.skipped++;
            continue;
        }
        if ((UINT64)(base + 4) * 64 > g_bone_width - g_bone_first * 64)
        {
            st.out_of_range++;
            continue;
        }
        m = (const float *)(bones + (size_t)base * 64);
        for (k = 0; k < 4; k++)
        {
            ident &= bone_is_identity(m + k * 16);
            zero &= bone_is_zero(m + k * 16);
        }
        if (ident && bases && *nbases < 64 && (!*nbases || bases[*nbases - 1] != base))
            bases[(*nbases)++] = base;   /* remember identity palettes to recheck later */
        if (ident)
            st.identity++;
        else if (zero)
            st.zero++;
        else
            st.other++;
        if (!zero)
        {
            const uint8_t *rec = *(const uint8_t *const *)(queue + (size_t)i * 24 + 0x10);
            int bit;

            if (rec && readable(rec, 48))
            {
                for (bit = 0; bit < 8; bit++)
                    if (rec[4] & (1 << bit))
                        (ident ? st.flag_identity : st.flag_animated)[bit]++;
                if (*(const int32_t *)(rec + 0x1c) != -1)
                    (ident ? st.remap_identity++ : st.remap_animated++);
                {
                    const uint8_t *mesh = *(const uint8_t *const *)(rec + 8);

                    if (mesh && readable(mesh, 0x38))
                    {
                        (ident ? st.mflags_id : st.mflags_an)[mesh[0x14]]++;
                        (ident ? st.m1c_id : st.m1c_an)[mesh[0x1c]]++;
                        (ident ? st.m34_id : st.m34_an)[mesh[0x34]]++;
                    }
                }
                if (sample && ident && st.identity <= 3)
                    log_record("IDENTITY", i, base, rec);
                if (sample && !ident && st.other <= 3)
                    log_record("animated", i, base, rec);
            }
            else
                (ident ? st.noflag_identity++ : st.noflag_animated++);
        }
        if (sample && ((ident && shown_id < 2) || (!ident && !zero && shown_anim < 2)))
        {
            log_msg("skin palettes:   item %d base %d record %p %s: bone0 %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f | t %.3f %.3f %.3f",
                    i, base, *(void *const *)(queue + (size_t)i * 24 + 0x10), ident ? "IDENTITY" : "animated",
                    m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
            if (ident)
                shown_id++;
            else
                shown_anim++;
        }
    }
    res->lpVtbl->Unmap(res, 0, &none);
    log_msg("skin palettes (%s): %u queued items, %u skipped (no palette), %u palettes ANIMATED, %u IDENTITY"
            " (bind pose), %u all-zero, %u out of range", when, st.items, st.skipped, st.other, st.identity, st.zero,
            st.out_of_range);
    log_msg("skin palettes: record flag bits (+4) among IDENTITY: b0=%u b1=%u b2=%u b3=%u b4=%u b5=%u b6=%u b7=%u"
            " (unreadable %u) | among ANIMATED: b0=%u b1=%u b2=%u b3=%u b4=%u b5=%u b6=%u b7=%u (unreadable %u)",
            st.flag_identity[0], st.flag_identity[1], st.flag_identity[2], st.flag_identity[3], st.flag_identity[4],
            st.flag_identity[5], st.flag_identity[6], st.flag_identity[7], st.noflag_identity,
            st.flag_animated[0], st.flag_animated[1], st.flag_animated[2], st.flag_animated[3], st.flag_animated[4],
            st.flag_animated[5], st.flag_animated[6], st.flag_animated[7], st.noflag_animated);
    log_msg("skin palettes: using a bone remap table (record +0x1c != -1): %u of %u IDENTITY, %u of %u ANIMATED",
            st.remap_identity, st.identity, st.remap_animated, st.other);
    log_histogram("mesh +0x14", st.mflags_id, st.mflags_an);
    log_histogram("mesh +0x1c", st.m1c_id, st.m1c_an);
    log_histogram("mesh +0x34", st.m34_id, st.m34_an);
    {
        const uint32_t *m = (const uint32_t *)(g_game_base + SKIN_MANAGER_RVA);

        if (readable(m, 0x80))
            log_msg("skin palettes: manager CPU-job requests +30=%u capacity +34=%u | +4c=%u +54=%u +70=%u +74=%u",
                    m[12], m[13], m[19], m[21], m[28], m[29]);
    }
}

/* The identity palettes again, later: were they filled after the dispatch was recorded? */
static DWORD g_palette_check_tick;

static void recheck_palettes_label(const char *when, const int32_t *bases, unsigned int nbases)
{
    ID3D12Resource *res = g_bone_res;
    D3D12_RANGE none = {0, 0};
    const uint8_t *bones = NULL;
    unsigned int i, ident = 0;

    if (!res || !nbases || FAILED(res->lpVtbl->Map(res, 0, &none, (void **)&bones)) || !bones)
        return;
    bones += g_bone_first * 64;
    for (i = 0; i < nbases; i++)
    {
        const float *m = (const float *)(bones + (size_t)bases[i] * 64);
        int k, all = 1;

        for (k = 0; k < 4; k++)
            all &= bone_is_identity(m + k * 16);
        ident += all;
    }
    res->lpVtbl->Unmap(res, 0, &none);
    log_msg("skin palettes (%s, +%lu ms): of %u identity palettes, %u still identity, %u now ANIMATED",
            when, (unsigned long)(GetTickCount() - g_palette_check_tick), nbases, ident, nbases - ident);
}

static void hk_skin_dispatch(void *a1, void *a2, void *a3, void *a4)
{
    static int32_t sampled[64];
    static unsigned int nsampled;
    static volatile LONG recheck_pending;
    static volatile LONG context_logged;
    int32_t n = *g_sb.count;
    LONG now, last;

    InterlockedIncrement64(&g_sb_dispatch_calls);
    if (n > 0)
    {
        InterlockedIncrement64(&g_sb_dispatch_work);
        /* entries needed are >= the queued item count; capacity below that drops work */
        if (g_sb.object && (uint32_t)n > g_sb.object[4])
            InterlockedIncrement64(&g_sb_dispatch_starved);
    }
    if (InterlockedIncrement(&g_sb_logged) <= 3)
        log_skin_dispatch("first calls");
    now = (LONG)GetTickCount();
    last = g_sb_last_tick;
    if (now - last > 3000 && InterlockedCompareExchange(&g_sb_last_tick, now, last) == last)
        log_skin_dispatch("periodic");
    {
        static volatile LONG view_last;
        LONG vl = view_last;

        /* every ~10 s, log the bindings and data of one skin dispatch with work */
        if (n > 0 && (now - vl >= (LONG)g_cfg.snapshot_ms || !vl) && InterlockedCompareExchange(&view_last, now, vl) == vl)
        {
            log_msg("skin window: snapshot (entries queued=%d)", (int)n);
            g_view_window_log = TRUE;
            log_readbacks();
            if (g_trap_handler)
                arm_writer_trap();
        }
    }
    if (InterlockedExchange(&recheck_pending, 0))
    {
        g_palette_submits = 0;
        recheck_palettes_label("one frame later", sampled, nsampled);
    }
    g_skin_window_tid = GetCurrentThreadId();
    g_dispatch_trampoline(a1, a2, a3, a4);
    g_skin_window_tid = 0;
    if (g_view_window_log && n > 0)
    {
        nsampled = 0;
        analyse_palettes("as the dispatch is recorded", n, sampled, &nsampled, 1);
        g_palette_check_tick = GetTickCount();
        g_palette_bases = sampled;
        g_palette_nbases = nsampled;
        g_palette_submits = nsampled ? 1 : 0;
        recheck_pending = nsampled > 0;
    }
    g_view_window_log = FALSE;
    if (n > 0 && !InterlockedExchange(&context_logged, 1))
        log_skin_context();
}

static void install_skin_budget(void)
{
    static const uint8_t prologue[13] = {
        0x4c, 0x8b, 0xdc, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00,
    };
    uint8_t *t = g_game_base + g_cfg.probe_sb_rva, *d = g_game_base + g_cfg.probe_dispatch_rva;

    if (!readable(t, 32) || memcmp(t, "\x44\x8b\x05", 3) || memcmp(t + 7, "\x48\x8d\x15", 3)
            || memcmp(t + 14, "\x41\xb9\x20\x00\x00\x00", 6) || memcmp(t + 20, "\x48\x8d\x0d", 3) || t[27] != 0xe9)
    {
        log_msg("skin_budget: SkinningSB stub bytes don't match this game build, not patching");
        return;
    }
    g_sb.budget = (int32_t *)(t + 7 + *(const int32_t *)(t + 3));
    g_sb.name = (const char *)(t + 14 + *(const int32_t *)(t + 10));
    g_sb.object = (uint32_t *)(t + 27 + *(const int32_t *)(t + 23));
    g_sb.create = (void *)(t + 32 + *(const int32_t *)(t + 28));
    if (!readable(g_sb.budget, 4) || !readable(g_sb.object, 0x18) || !readable(g_sb.name, 11)
            || memcmp(g_sb.name, "SkinningSB", 11))
    {
        log_msg("skin_budget: SkinningSB stub targets look wrong, not patching");
        return;
    }
    log_msg("skin_budget: SkinningSB stub at %p: budget %p object %p (capacity now %u) create %p; minimum %u",
            (void *)t, (void *)g_sb.budget, (void *)g_sb.object, g_sb.object[4], (void *)g_sb.create, g_cfg.skin_budget);
    if (g_sb.object[4])
        log_msg("skin_budget: SkinningSB already exists, the budget can only apply if the game recreates it");
    if (!patch_prologue(t, 12, hk_skin_sb_create))
        return;

    /* dispatch function: prologue, then movsxd rsi, [queued entry count] */
    if (!readable(d, 20) || memcmp(d, prologue, 13) || memcmp(d + 13, "\x48\x63\x35", 3))
    {
        log_msg("skin_budget: skin dispatch function doesn't match this game build, not hooked");
        return;
    }
    g_sb.count = (int32_t *)(d + 20 + *(const int32_t *)(d + 16));
    /* +0x16: 39 05 disp (cmp [selector], eax); +0x23: 0f 45 05 disp (cmovne eax, [alt]);
     * +0x30: 48 8b 05 disp (mov rax, [queue array]) */
    if (readable(d + 0x16, 0x21) && !memcmp(d + 0x16, "\x39\x05", 2) && !memcmp(d + 0x23, "\x0f\x45\x05", 3)
            && !memcmp(d + 0x30, "\x48\x8b\x05", 3))
    {
        g_queue_sel = (const int32_t *)(d + 0x1c + *(const int32_t *)(d + 0x18));
        g_queue_alt = (const int32_t *)(d + 0x2a + *(const int32_t *)(d + 0x26));
        g_queue_base_ptr = d + 0x37 + *(const int32_t *)(d + 0x33);
    }
    if (readable(g_sb.count, 4) && (g_dispatch_trampoline = patch_prologue(d, 13, hk_skin_dispatch)))
        log_msg("skin_budget: hooked skin dispatch at %p (queued count at %p)", (void *)d, (void *)g_sb.count);
}

/* ------------------------------------------------------------ writer trap */

/* The skinning manager keeps a per-frame list of palette copies (manager +0x58, count
 * +0x60), 16 bytes each: { u32 palette byte offset, u32 byte size, CPU source pointer }.
 * For broken meshes the CPU source matrices are never computed. To find the code that
 * computes them for working characters, guard the page holding one animated source and
 * log the first write into it: instruction address and call chain (as game RVAs). Faults
 * from other accesses to that page are stepped over (trap flag) and the guard re-armed. */
static volatile LONG g_trap_armed, g_trap_hits, g_trap_misses;
static uint8_t *volatile g_trap_lo, *volatile g_trap_hi;
static uint8_t *volatile g_trap_page;
static volatile DWORD g_trap_rearm_tid;

static unsigned long long game_rva(DWORD64 va)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_game_base + ((IMAGE_DOS_HEADER *)g_game_base)->e_lfanew);

    return va >= (DWORD64)g_game_base && va < (DWORD64)g_game_base + nt->OptionalHeader.SizeOfImage
            ? (unsigned long long)(va - (DWORD64)g_game_base) : 0ull;
}

static void log_call_chain(const CONTEXT *start, const char *what)
{
    CONTEXT ctx = *start;
    char line[512];
    size_t len = 0;
    int depth;

    len += snprintf(line + len, sizeof(line) - len, "%#llx", game_rva(ctx.Rip));
    for (depth = 0; depth < 10; depth++)
    {
        DWORD64 image = 0, frame = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        void *handler_data = NULL;

        if (!fn)
        {
            /* leaf function: return address is at [rsp] */
            if (!readable((void *)ctx.Rsp, 8))
                break;
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
        }
        else
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, NULL);
        if (!ctx.Rip)
            break;
        len += snprintf(line + len, sizeof(line) - len, " <- %#llx", game_rva(ctx.Rip));
        if (len > sizeof(line) - 32)
            break;
    }
    log_msg("trap_writer: %s at game RVA %s (0 = outside the game exe)", what, line);
}

static LONG CALLBACK trap_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD old;

    if (er->ExceptionCode == STATUS_SINGLE_STEP && g_trap_rearm_tid == GetCurrentThreadId())
    {
        /* the access we let through has completed: guard the page again */
        g_trap_rearm_tid = 0;
        ep->ContextRecord->EFlags &= ~0x100u;
        if (g_trap_armed && g_trap_page)
            VirtualProtect(g_trap_page, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || !g_trap_armed || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    {
        uint8_t *addr = (uint8_t *)er->ExceptionInformation[1];

        if ((uint8_t *)((ULONG_PTR)addr & ~(ULONG_PTR)4095) != g_trap_page)
            return EXCEPTION_CONTINUE_SEARCH;
        /* the guard is cleared by the fault itself */
        if (g_trap_misses < 3)
            log_msg("trap_writer: guard fault kind=%llu addr=%p rip=%#llx", (unsigned long long)er->ExceptionInformation[0],
                    (void *)addr, game_rva(ep->ContextRecord->Rip));
        if (er->ExceptionInformation[0] == 1 && addr >= g_trap_lo && addr < g_trap_hi)
        {
            g_trap_armed = 0;
            InterlockedIncrement(&g_trap_hits);
            log_call_chain(ep->ContextRecord, "a bone-matrix WRITE");
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (InterlockedIncrement(&g_trap_misses) > 5000)
        {
            g_trap_armed = 0;   /* too busy a page: give up on it */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* someone else touched the page: let this instruction run, then re-arm */
        g_trap_rearm_tid = GetCurrentThreadId();
        ep->ContextRecord->EFlags |= 0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

/* Called at a palette snapshot: look at this frame's copy list, log one animated and one
 * identity entry's source, and guard the animated one's source page. */
static void arm_writer_trap(void)
{
    const uint8_t *mgr = g_game_base + SKIN_MANAGER_RVA;
    ID3D12Resource *res = g_bone_res;
    D3D12_RANGE none = {0, 0};
    const uint8_t *bones = NULL, *list;
    int32_t n, i, shown_id = 0;
    DWORD old;

    if (!res || !readable(mgr, 0x68) || g_trap_hits >= 3 || g_trap_armed)
        return;
    list = *(const uint8_t *const *)(mgr + 0x58);
    n = *(const int32_t *)(mgr + 0x60);
    if (!list || n <= 0 || !readable(list, (size_t)n * 16))
        return;
    if (FAILED(res->lpVtbl->Map(res, 0, &none, (void **)&bones)) || !bones)
        return;
    bones += g_bone_first * 64;
    for (i = 0; i < n; i++)
    {
        uint32_t dst = *(const uint32_t *)(list + (size_t)i * 16), size = *(const uint32_t *)(list + (size_t)i * 16 + 4);
        uint8_t *src = *(uint8_t *const *)(list + (size_t)i * 16 + 8);
        int ident;

        if (!src || size < 64 || dst + 64 > g_bone_width || !readable(src, 64))
            continue;
        ident = bone_is_identity((const float *)(bones + dst));
        if (ident && shown_id < 2)
        {
            const float *m = (const float *)src;

            shown_id++;
            log_msg("trap_writer: IDENTITY palette %u (%u bones): CPU source %p starts %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                    dst / 64, size / 64, (void *)src, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        }
        if (!ident && !g_trap_armed)
        {
            uint8_t *page = (uint8_t *)((ULONG_PTR)src & ~(ULONG_PTR)4095);

            g_trap_lo = src;
            g_trap_hi = src + (size < 4096 ? size : 4096);
            g_trap_page = page;
            g_trap_misses = 0;
            g_trap_armed = 1;
            if (!VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &old))
            {
                g_trap_armed = 0;
                log_msg("trap_writer: could not guard %p (error %lu)", (void *)page, GetLastError());
            }
            else
                log_msg("trap_writer: armed on animated palette %u (%u bones), CPU source %p", dst / 64, size / 64,
                        (void *)src);
        }
    }
    res->lpVtbl->Unmap(res, 0, &none);
}

static void install_writer_trap(void)
{
    if ((g_trap_handler = AddVectoredExceptionHandler(1, trap_handler)))
        log_msg("trap_writer: exception handler installed (manager at %p)", (void *)(g_game_base + SKIN_MANAGER_RVA));
}

/* ------------------------------------------------------------ force_anim */

/* The game decides per actor whether to evaluate its animation this frame, from data
 * objects loaded with each asset (layouts from the game's reflection tables):
 *   AnimUpdateLODPrius            +0x0c RangeBase, +0x10 RangeVisible (float)
 *   AnimControllerComponentPrius  +0x60 MaxUpdateDistance (0 = always), +0x68 NeverCull,
 *                                 +0x6a AutoFreezeSkinning (bool)
 *   SimpleAnimComponentPrius      +0x20 MaxUpdateDistance, +0x24 MaxCulledUpdateDistance
 * Meshes whose animation isn't evaluated keep identity bone palettes (bind pose). With
 * force_anim, a background thread finds these objects by their vtable (checked against
 * the RTTI class name) and sets every one to "always animate, never cull, never freeze". */
struct anim_class
{
    const char *rtti;           /* RTTI type name */
    uint32_t vtbl_rva;          /* in the analysed build; verified at runtime */
    void *vtbl;
    unsigned int found, patched;
};
static struct anim_class g_anim_classes[3] = {
    { ".?AVAnimUpdateLODPrius@@", 0, NULL, 0, 0 },
    { ".?AVAnimControllerComponentPrius@@", 0, NULL, 0, 0 },
    { ".?AVSimpleAnimComponentPrius@@", 0, NULL, 0, 0 },
};

/* vtable[-1] is the RTTI complete-object locator; its type descriptor holds the name */
static int vtable_has_rtti(const uint8_t *vt, const char *name)
{
    const uint8_t *col, *td;
    size_t len = strlen(name) + 1;

    if (!readable(vt - 8, 8) || !(col = *(const uint8_t *const *)(vt - 8)) || !readable(col, 24))
        return 0;
    td = g_game_base + *(const uint32_t *)(col + 12);
    return readable(td + 0x10, len) && !memcmp(td + 0x10, name, len);
}

static int finite_range(float f, float lo, float hi)
{
    return f == f && f >= lo && f <= hi;
}

/* Decides the new field values for one object (a copy in o); returns 1 if anything changes. */
static int patch_anim_object(int cls, uint8_t *o)
{
    int c = 0;

    switch (cls)
    {
    case 0: /* AnimUpdateLODPrius */
        if (!finite_range(*(float *)(o + 8), 0, 1e7f) || !finite_range(*(float *)(o + 0xc), 0, 1e7f)
                || !finite_range(*(float *)(o + 0x10), 0, 1e7f) || o[0x14] > 1)
            return 0;
        if (*(float *)(o + 0xc) < 1e6f) { *(float *)(o + 0xc) = 1e6f; c = 1; }
        if (*(float *)(o + 0x10) < 1e6f) { *(float *)(o + 0x10) = 1e6f; c = 1; }
        break;
    case 1: /* AnimControllerComponentPrius */
        if (!finite_range(*(float *)(o + 0x60), 0, 1e7f) || !finite_range(*(float *)(o + 0x64), 0, 1e7f)
                || o[0x68] > 1 || o[0x69] > 1 || o[0x6a] > 1)
            return 0;
        if (*(float *)(o + 0x60) != 0.0f) { *(float *)(o + 0x60) = 0.0f; c = 1; }
        if (!o[0x68]) { o[0x68] = 1; c = 1; }
        if (o[0x6a]) { o[0x6a] = 0; c = 1; }
        break;
    case 2: /* SimpleAnimComponentPrius */
        if (!finite_range(*(float *)(o + 0x20), 0, 1e7f) || !finite_range(*(float *)(o + 0x24), 0, 1e7f)
                || o[0x28] > 1 || o[0x29] > 1)
            return 0;
        if (*(float *)(o + 0x20) != 0.0f) { *(float *)(o + 0x20) = 0.0f; c = 1; }
        if (*(float *)(o + 0x24) != 0.0f) { *(float *)(o + 0x24) = 0.0f; c = 1; }
        break;
    }
    return c;
}

static void log_anim_object(int cls, const uint8_t *addr, const uint8_t *o)
{
    static volatile LONG shown[3];

    if (InterlockedIncrement(&shown[cls]) > 3)
        return;
    if (cls == 0)
        log_msg("force_anim: AnimUpdateLODPrius %p: PlayAnimBump=%g RangeBase=%g RangeVisible=%g BumpOnActivate=%u",
                (const void *)addr, *(const float *)(o + 8), *(const float *)(o + 0xc), *(const float *)(o + 0x10), o[0x14]);
    else if (cls == 1)
        log_msg("force_anim: AnimControllerComponentPrius %p: MaxUpdateDistance=%g NeverCull=%u BindAfterAnimEnd=%u"
                " AutoFreezeSkinning=%u", (const void *)addr, *(const float *)(o + 0x60), o[0x68], o[0x69], o[0x6a]);
    else
        log_msg("force_anim: SimpleAnimComponentPrius %p: MaxUpdateDistance=%g MaxCulledUpdateDistance=%g",
                (const void *)addr, *(const float *)(o + 0x20), *(const float *)(o + 0x24));
}

/* Memory can be freed while we scan, so read through ReadProcessMemory (fails cleanly
 * instead of faulting) and write through WriteProcessMemory after re-reading. */
static void scan_anim_objects(void)
{
    static uint8_t chunk[(1 << 20) + 0x100];
    HANDLE self = GetCurrentProcess();
    MEMORY_BASIC_INFORMATION info;
    uint8_t *addr = (uint8_t *)0x10000;
    unsigned int found[3] = {0}, changed = 0, i;
    SIZE_T scanned = 0;
    DWORD t0 = GetTickCount();

    while (VirtualQuery(addr, &info, sizeof(info)) == sizeof(info))
    {
        uint8_t *base = info.BaseAddress, *next = base + info.RegionSize, *p;

        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))
                && (info.Protect & PAGE_READWRITE) && info.RegionSize <= ((SIZE_T)1 << 31))
        {
            for (p = base; p < next; p += 1 << 20)
            {
                SIZE_T want = (SIZE_T)(next - p) < (1 << 20) + 0x80 ? (SIZE_T)(next - p) : (1 << 20) + 0x80, got = 0;
                SIZE_T k;

                if (!ReadProcessMemory(self, p, chunk, want, &got) || got < 0x80)
                    continue;
                scanned += got;
                for (k = 0; k + 0x80 <= got && k < (1 << 20); k += 8)
                {
                    uint64_t v = *(const uint64_t *)(chunk + k);
                    int cls;

                    for (cls = 0; cls < 3; cls++)
                    {
                        uint8_t obj[0x80];

                        if (!g_anim_classes[cls].vtbl || v != (uint64_t)g_anim_classes[cls].vtbl)
                            continue;
                        found[cls]++;
                        memcpy(obj, chunk + k, sizeof(obj));
                        log_anim_object(cls, p + k, obj);
                        if (patch_anim_object(cls, obj))
                        {
                            uint8_t now[0x80];
                            SIZE_T n = 0;

                            /* still the same object? then write the changed fields back */
                            if (ReadProcessMemory(self, p + k, now, sizeof(now), &n) && n == sizeof(now)
                                    && *(const uint64_t *)now == v
                                    && WriteProcessMemory(self, p + k + 8, obj + 8, sizeof(obj) - 8, &n))
                                changed++;
                        }
                    }
                }
            }
        }
        if (next <= addr)
            break;
        addr = next;
    }
    for (i = 0; i < 3; i++)
        g_anim_classes[i].found = found[i];
    log_msg("force_anim: scanned %llu MB in %lu ms: AnimUpdateLODPrius=%u AnimControllerComponentPrius=%u"
            " SimpleAnimComponentPrius=%u, %u changed now", (unsigned long long)(scanned >> 20),
            (unsigned long)(GetTickCount() - t0), found[0], found[1], found[2], changed);
}

static DWORD WINAPI force_anim_thread(void *arg)
{
    unsigned int round = 0;

    Sleep(g_cfg.force_anim_delay_ms);
    for (;;)
    {
        if (g_exiting)
            return 0;
        scan_anim_objects();
        Sleep(++round < 6 ? 10000 : 30000);
    }
}

static void install_force_anim(void)
{
    int i, ok = 0;

    for (i = 0; i < 3; i++)
    {
        uint8_t *vt = g_game_base + g_cfg.anim_vtbl_rva[i];

        if (vtable_has_rtti(vt, g_anim_classes[i].rtti))
        {
            g_anim_classes[i].vtbl = vt;
            ok++;
        }
        else
            log_msg("force_anim: %s vtable not found at the expected place (other game build?)", g_anim_classes[i].rtti);
    }
    if (ok && CreateThread(NULL, 0, force_anim_thread, NULL, 0, NULL))
        log_msg("force_anim: watching for %d animation config classes", ok);
}

/* ---------------------------------------------------------- Ziva RT CPU path */

/* Ziva RT (the ML face and cloth deformer) has three CPU implementations: AVX2, SSE4.1 and
 * a plain reference one. The game picks one at startup from CPUID (function at
 * ziva_level_rva: 2 = AVX+AVX2, else the SSE4.1 bit) and stores it as the vtable of a
 * global object. Under Rosetta, what CPUID advertises (and how well those instructions are
 * translated) differs between Apple chips. This logs what the game sees and picks, and with
 * ziva_cpu set keeps the chosen implementation in place. */
static const char *const ziva_names[3] = { "Reference", "SSE4.1", "AVX2" };
static const char *const ziva_rtti[3] =
{
    ".?AUReferenceImpl@?A0xaf88766b@ZivaRT@@",
    ".?AU?$SIMDImpl@USSE4_1Impl@?A0xaf88766b@ZivaRT@@@?A0xaf88766b@ZivaRT@@",
    ".?AU?$SIMDImpl@UAVX2Impl@?A0xaf88766b@ZivaRT@@@?A0xaf88766b@ZivaRT@@",
};
static const uint8_t ziva_level_bytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x10, 0x33, 0xc9, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0xa2 };
static uint8_t *g_ziva_vtbl[3];
static uint8_t **g_ziva_obj;

static int ziva_which(const uint8_t *vt)
{
    int i;

    for (i = 0; i < 3; i++)
        if (vt && vt == g_ziva_vtbl[i])
            return i;
    return -1;
}

static DWORD WINAPI ziva_thread(void *arg)
{
    int want = g_cfg.ziva_cpu - 1, last = -2;
    unsigned int round = 0;
    LONG switches = 0;

    for (;; round++)
    {
        uint8_t *vt;
        int cur;

        if (g_exiting)
            return 0;
        vt = *(uint8_t *volatile *)g_ziva_obj;
        cur = ziva_which(vt);
        if (want >= 0 && cur >= 0 && cur != want)
        {
            *(uint8_t *volatile *)g_ziva_obj = g_ziva_vtbl[want];
            if (++switches <= 3)
                log_msg("ziva_cpu: game had picked %s, switched it to %s", ziva_names[cur], ziva_names[want]);
            cur = want;
        }
        if (cur != last)
        {
            log_msg("ziva: Ziva RT now runs its %s CPU code%s", cur >= 0 ? ziva_names[cur] : "(not set yet)",
                    vt && cur < 0 ? " (unknown vtable!)" : "");
            last = cur;
        }
        Sleep(round < 1200 ? 50 : 1000);
    }
}

static void install_ziva(void)
{
    int regs[4], max_leaf, (*level_fn)(void);
    unsigned int lo = 0, hi = 0;
    uint8_t *level = g_game_base + g_cfg.ziva_level_rva;
    int i, ok = 1;

    __cpuid(regs, 0);
    max_leaf = regs[0];
    __cpuid(regs, 1);
    log_msg("cpu: leaf1 ecx=%08x edx=%08x (SSE4.1=%d SSE4.2=%d AVX=%d FMA=%d F16C=%d OSXSAVE=%d MOVBE=%d POPCNT=%d)",
            regs[2], regs[3], regs[2] >> 19 & 1, regs[2] >> 20 & 1, regs[2] >> 28 & 1, regs[2] >> 12 & 1,
            regs[2] >> 29 & 1, regs[2] >> 27 & 1, regs[2] >> 22 & 1, regs[2] >> 23 & 1);
    if (regs[2] >> 27 & 1)
    {
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        log_msg("cpu: XCR0=%08x%08x (SSE state=%u, AVX state=%u, AVX-512 state=%u)", hi, lo, lo >> 1 & 1, lo >> 2 & 1,
                (lo >> 5 & 7) == 7);
    }
    if (max_leaf >= 7)
    {
        __cpuidex(regs, 7, 0);
        log_msg("cpu: leaf7 ebx=%08x ecx=%08x edx=%08x (AVX2=%d BMI1=%d BMI2=%d AVX-512F=%d)", regs[1], regs[2], regs[3],
                regs[1] >> 5 & 1, regs[1] >> 3 & 1, regs[1] >> 8 & 1, regs[1] >> 16 & 1);
        __cpuidex(regs, 7, 1);
        log_msg("cpu: leaf7.1 eax=%08x (AVX-VNNI=%d AVX-IFMA=%d)", regs[0], regs[0] >> 4 & 1, regs[0] >> 23 & 1);
    }
    __cpuid(regs, 0x80000001);
    log_msg("cpu: ext leaf ecx=%08x (LZCNT=%d)", regs[2], regs[2] >> 5 & 1);

    if (!readable(level, sizeof(ziva_level_bytes)) || memcmp(level, ziva_level_bytes, sizeof(ziva_level_bytes)))
    {
        log_msg("ziva: CPU level function doesn't match this game build, not touching Ziva");
        return;
    }
    level_fn = (int (*)(void))(void *)level;
    i = level_fn();
    log_msg("ziva: the game's CPU check says level %d -> it uses Ziva's %s code", i,
            i == 2 ? "AVX2" : i == 1 ? "SSE4.1" : "Reference");
    for (i = 0; i < 3; i++)
    {
        g_ziva_vtbl[i] = g_game_base + g_cfg.ziva_vtbl_rva[i];
        if (!vtable_has_rtti(g_ziva_vtbl[i], ziva_rtti[i]))
        {
            log_msg("ziva: %s vtable not at the expected place (other game build?)", ziva_names[i]);
            ok = 0;
        }
    }
    g_ziva_obj = (uint8_t **)(g_game_base + g_cfg.ziva_impl_rva);
    if (!ok || !readable(g_ziva_obj, 8))
        return;
    if (g_cfg.ziva_cpu)
        log_msg("ziva_cpu: forcing Ziva's %s code", ziva_names[g_cfg.ziva_cpu - 1]);
    CreateThread(NULL, 0, ziva_thread, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        WCHAR path[MAX_PATH];

        g_self = instance;
        DisableThreadLibraryCalls(instance);
        /* Config and log are needed this early so the CPU-count override is in place
         * before the game's own startup code runs. Nothing here loads other DLLs. */
        path_next_to_self(path, MAX_PATH, L"sm2fix.log");
        /* Append: tests often need several launches in one log. */
        g_log = _wfopen(path, L"a");
        load_config();
        log_msg("========== new launch ==========");
        log_msg("attach: %u threads already in the process", sm2_count_process_threads());
        if (g_cfg.cpu_count)
            log_msg("cpu_count=%u: patched %u imports of the game", g_cfg.cpu_count,
                    sm2_install_cpu_limit(g_cfg.cpu_count));
        g_game_base = (uint8_t *)GetModuleHandleA(NULL);
        if (g_cfg.probe_skin)
            install_skin_probe();
        if (g_cfg.probe_skin || g_cfg.skin_budget)
            install_skin_budget();
        if (g_cfg.force_anim)
            install_force_anim();
        if (g_cfg.trap_writer)
            install_writer_trap();
        install_ziva();
    }
    else if (reason == DLL_PROCESS_DETACH && g_log)
    {
        g_exiting = TRUE;
        if (g_dispatch_trampoline)
            fprintf(g_log, "exit: skin dispatch calls=%lld with-work=%lld over-capacity=%lld capacity=%u;"
                    " inside: SetPipelineState=%lld (skinning=%lld) Dispatch=%lld (skinning=%lld) ExecuteIndirect=%lld\n",
                    (long long)g_sb_dispatch_calls, (long long)g_sb_dispatch_work,
                    (long long)g_sb_dispatch_starved, g_sb.object[4], (long long)g_win_set_pso,
                    (long long)g_win_set_pso_skin, (long long)g_win_dispatch, (long long)g_win_dispatch_skin,
                    (long long)g_win_indirect);
        if (g_skin_trampoline)
            fprintf(g_log, "exit: skin launcher calls=%lld with-requests=%lld blocked-by-flag=%lld\n",
                    (long long)g_skin_calls, (long long)g_skin_with_requests, (long long)g_skin_flag_blocked);
        if (g_cfg.skip_skinning)
            fprintf(g_log, "exit: skip_skinning dropped %lld skinning dispatches\n", (long long)g_skin_skipped);
        if (g_cfg.profile)
            log_profile("exit", 40);
        fprintf(g_log, "exit: %lld forced barriers, %lld target PSOs\n",
                (long long)g_barriers, (long long)g_target_psos);
        fprintf(g_log, "exit: root signatures=%lld with min/max static samplers=%lld; heap samplers=%lld min/max=%lld\n",
                (long long)g_rootsigs, (long long)g_minmax_static, (long long)g_samplers_heap, (long long)g_minmax_heap);
        fprintf(g_log, "exit: timestamp resolves faked=%lld passed-through=%lld\n",
                (long long)g_ts_faked_resolves, (long long)g_ts_fallback_resolves);
        fprintf(g_log, "exit: dispatches=%lld execute_indirect=%lld pso_failures=%lld\n",
                (long long)g_dispatches, (long long)g_indirect, (long long)g_pso_failures);
        fprintf(g_log, "exit: submits direct=%lld compute=%lld copy=%lld; fence signals=%lld waits=%lld;"
                " serialize waits=%lld timeouts=%lld\n",
                (long long)g_submits[0], (long long)g_submits[2], (long long)g_submits[3],
                (long long)g_fence_signals, (long long)g_fence_waits, (long long)g_waits, (long long)g_wait_timeouts);
        fclose(g_log);
        g_log = NULL;
    }
    return TRUE;
}
