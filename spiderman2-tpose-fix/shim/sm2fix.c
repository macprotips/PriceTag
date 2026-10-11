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
#include <stdio.h>
#include <stdarg.h>

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
    if ((e = find_list_locked(list, target || g_cfg.profile)))
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
        e->list = TOMBSTONE;
    ReleaseSRWLockExclusive(&g_lock);
}

/* -------------------------------------------------------------- profiling */

/* The body-skinning compute shader found by decompiling the game's shaders: reads up to
 * 8 bone influences per vertex and writes posed, quantized vertices to a buffer. */
#define SM2_SKINNING_HASH 0x3d50acdb571dce22ull
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
    for (i = 0; i < n; i++)
    {
        if (copy[i].hash == SM2_SKINNING_HASH)
            skin_d = copy[i].dispatches, skin_i = copy[i].indirect, skin_g = copy[i].groups;
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
    if (g_cfg.profile)
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

static void hook_command_list_object(void *list);

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
        list_bind_pso(list, pso);
    return hr;
}

static void STDMETHODCALLTYPE hk_list_ClearState(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12PipelineState *)
            = orig_fn(list, CL_IDX(ClearState));

    orig(list, pso);
    list_bind_pso(list, pso);
}

static void STDMETHODCALLTYPE hk_list_SetPipelineState(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, ID3D12PipelineState *)
            = orig_fn(list, CL_IDX(SetPipelineState));

    orig(list, pso);
    list_bind_pso(list, pso);
}

static void STDMETHODCALLTYPE hk_list_Dispatch(ID3D12GraphicsCommandList *list, UINT x, UINT y, UINT z)
{
    void (STDMETHODCALLTYPE *orig)(ID3D12GraphicsCommandList *, UINT, UINT, UINT)
            = orig_fn(list, CL_IDX(Dispatch));

    orig(list, x, y, z);
    InterlockedIncrement64(&g_dispatches);
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

static HRESULT STDMETHODCALLTYPE hk_dev_CreateComputePipelineState(ID3D12Device *device,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pso)
{
    HRESULT (STDMETHODCALLTYPE *orig)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)
            = orig_fn(device, DEV_IDX(CreateComputePipelineState));
    HRESULT hr;

    pso_creation_delay();
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
            note_compute_shader(*pso, code, size, "CreatePipelineState");
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

    orig(queue, count, lists);
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
#define SKIN_MANAGER_RVA  0xbf60300
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

static void install_skin_probe(void)
{
    uint8_t *target, *tramp;
    DWORD old;

    g_game_base = (uint8_t *)GetModuleHandleA(NULL);
    target = g_game_base + g_cfg.probe_skin_rva;
    if (!readable(target, 15) || memcmp(target, skin_launcher_bytes, 15))
    {
        log_msg("probe_skin: launcher bytes don't match this game build, not patching");
        return;
    }
    if (!(tramp = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        return;
    /* trampoline: original 15 bytes, then mov rax, target+15; jmp rax */
    memcpy(tramp, target, 15);
    tramp[15] = 0x48; tramp[16] = 0xb8;
    *(uint64_t *)(tramp + 17) = (uint64_t)(target + 15);
    tramp[25] = 0xff; tramp[26] = 0xe0;
    g_skin_trampoline = (void *)tramp;
    FlushInstructionCache(GetCurrentProcess(), tramp, 27);

    if (!VirtualProtect(target, 15, PAGE_EXECUTE_READWRITE, &old))
        return;
    /* mov rax, hk_skin_launcher; jmp rax; nop x3 (rax is free at function entry) */
    {
        uint8_t patch[15] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0, 0x90, 0x90, 0x90};
        *(uint64_t *)(patch + 2) = (uint64_t)hk_skin_launcher;
        memcpy(target, patch, 15);
    }
    VirtualProtect(target, 15, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 15);
    log_msg("probe_skin: hooked skin launcher at %p (manager global at %p)", (void *)target,
            (void *)(g_game_base + SKIN_MANAGER_RVA));
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
        if (g_cfg.probe_skin)
            install_skin_probe();
    }
    else if (reason == DLL_PROCESS_DETACH && g_log)
    {
        g_exiting = TRUE;
        if (g_skin_trampoline)
            fprintf(g_log, "exit: skin launcher calls=%lld with-requests=%lld blocked-by-flag=%lld\n",
                    (long long)g_skin_calls, (long long)g_skin_with_requests, (long long)g_skin_flag_blocked);
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
