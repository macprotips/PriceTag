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
static volatile LONG64 g_barriers, g_target_psos, g_dispatches, g_indirect, g_submits[4], g_waits,
        g_wait_timeouts, g_pso_failures, g_fence_signals, g_fence_waits;

/* ------------------------------------------------------------------------ logging */

static void log_msg(const char *fmt, ...)
{
    va_list args;

    if (!g_log)
        return;
    AcquireSRWLockExclusive(&g_lock);
    va_start(args, fmt);
    fprintf(g_log, "[%lu] ", GetTickCount());
    vfprintf(g_log, fmt, args);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(args);
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
    if (g_hook_count == ARRAYSIZE(g_hooks) || vtbl[index] == hook)
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
    if ((e = find_list_locked(list, target)))
        e->target_bound = target;
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

/* ------------------------------------------------------------- shader matching */

static void note_compute_shader(void *pso, const void *code, size_t size, const char *how)
{
    uint64_t hash;

    if (!pso || !code || !size)
        return;
    hash = sm2_shader_hash(code, size);
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
static void dump_shader(const char *stage, const void *code, size_t size)
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
    log_msg("  %s shader %016llx (%zu bytes)", stage, (unsigned long long)hash, size);
    if (InterlockedIncrement(&dumps) > 200)
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

static void hook_command_list_vtable(void *list)
{
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
    HRESULT hr = orig(device, desc, riid, pso);

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
    HRESULT hr = orig(device, desc, riid, pso);
    const void *code = NULL;
    size_t size = 0;

    if (SUCCEEDED(hr) && desc && pso && *pso)
    {
        enum sm2_stream_result r = sm2_stream_find_cs(desc->pPipelineStateSubobjectStream,
                desc->SizeInBytes, &code, &size);

        if (r == SM2_STREAM_COMPUTE)
            note_compute_shader(*pso, code, size, "CreatePipelineState");
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
    HRESULT hr = orig(device, desc, riid, pso);

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
    HRESULT hr = orig(device, node_mask, blob, size, riid, out);

    if (FAILED(hr))
        log_msg("FAILED CreateRootSignature hr=%#lx (%zu bytes)", (unsigned long)hr, (size_t)size);
    return hr;
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
    char what[64];

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

    path_next_to_self(path, MAX_PATH, L"sm2fix.log");
    g_log = _wfopen(path, L"w");
    load_config();

    log_msg("sm2fix loaded: mode=%s log_shaders=%d hashes=%u",
            mode_names[g_cfg.mode], g_cfg.log_shaders, g_cfg.hash_count);
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

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    }
    else if (reason == DLL_PROCESS_DETACH && g_log)
    {
        fprintf(g_log, "exit: %lld forced barriers, %lld target PSOs\n",
                (long long)g_barriers, (long long)g_target_psos);
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
