/* A stand-in for the translation layer's d3d12.dll (D3DMetal, vkd3d...). Implements just
 * enough of ID3D12Device / ID3D12GraphicsCommandList for the shim to hook, and records
 * every ResourceBarrier so the test can check what the shim injected. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

static LONG g_uav_barriers, g_other_barriers, g_dispatches, g_signals, g_executes, g_indirect;

void mock_get_queue_counts(LONG *signals, LONG *executes, LONG *indirect)
{
    *signals = g_signals;
    *executes = g_executes;
    *indirect = g_indirect;
}

void mock_get_counts(LONG *uav, LONG *other, LONG *dispatches)
{
    *uav = g_uav_barriers;
    *other = g_other_barriers;
    *dispatches = g_dispatches;
}

/* ---- generic refcounted object used for PSOs */
struct object { IUnknownVtbl *vtbl; LONG refs; };

static HRESULT STDMETHODCALLTYPE obj_QI(IUnknown *iface, REFIID riid, void **out) { *out = iface; return S_OK; }
static ULONG STDMETHODCALLTYPE obj_AddRef(IUnknown *iface) { return InterlockedIncrement(&((struct object *)iface)->refs); }
static ULONG STDMETHODCALLTYPE obj_Release(IUnknown *iface)
{
    struct object *o = (struct object *)iface;
    ULONG r = InterlockedDecrement(&o->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, o);
    return r;
}
static IUnknownVtbl obj_vtbl = {obj_QI, obj_AddRef, obj_Release};

static void *new_object(void)
{
    struct object *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*o));
    o->vtbl = &obj_vtbl;
    o->refs = 1;
    return o;
}

/* ---- command list */
struct list { ID3D12GraphicsCommandListVtbl *vtbl; LONG refs; };

static HRESULT STDMETHODCALLTYPE list_QI(ID3D12GraphicsCommandList *iface, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_ID3D12GraphicsCommandList) || IsEqualGUID(riid, &IID_ID3D12GraphicsCommandList1)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        iface->lpVtbl->AddRef(iface);
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE list_AddRef(ID3D12GraphicsCommandList *iface) { return InterlockedIncrement(&((struct list *)iface)->refs); }
static ULONG STDMETHODCALLTYPE list_Release(ID3D12GraphicsCommandList *iface)
{
    struct list *l = (struct list *)iface;
    ULONG r = InterlockedDecrement(&l->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, l);
    return r;
}
static HRESULT STDMETHODCALLTYPE list_Reset(ID3D12GraphicsCommandList *iface, ID3D12CommandAllocator *a, ID3D12PipelineState *p) { return S_OK; }
static void STDMETHODCALLTYPE list_ClearState(ID3D12GraphicsCommandList *iface, ID3D12PipelineState *p) {}
static void STDMETHODCALLTYPE list_SetPipelineState(ID3D12GraphicsCommandList *iface, ID3D12PipelineState *p) {}
static void STDMETHODCALLTYPE list_Dispatch(ID3D12GraphicsCommandList *iface, UINT x, UINT y, UINT z) { InterlockedIncrement(&g_dispatches); }
static void STDMETHODCALLTYPE list_ResourceBarrier(ID3D12GraphicsCommandList *iface, UINT n, const D3D12_RESOURCE_BARRIER *b)
{
    UINT i;
    for (i = 0; i < n; i++)
    {
        if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && !b[i].UAV.pResource)
            InterlockedIncrement(&g_uav_barriers);
        else
            InterlockedIncrement(&g_other_barriers);
    }
}

static void STDMETHODCALLTYPE list_ExecuteIndirect(ID3D12GraphicsCommandList *iface, ID3D12CommandSignature *sig,
        UINT max, ID3D12Resource *args, UINT64 off, ID3D12Resource *cnt, UINT64 cnt_off)
{
    InterlockedIncrement(&g_indirect);
}

static ID3D12GraphicsCommandListVtbl list_vtbl;

/* ---- fence: completes asynchronously ~5 ms after a queue signals it, like a GPU */
struct fence { ID3D12FenceVtbl *vtbl; LONG refs; volatile LONG64 completed; UINT64 wait_value; HANDLE wait_event; };

static HRESULT STDMETHODCALLTYPE fence_QI(ID3D12Fence *iface, REFIID riid, void **out) { *out = iface; return S_OK; }
static ULONG STDMETHODCALLTYPE fence_AddRef(ID3D12Fence *iface) { return InterlockedIncrement(&((struct fence *)iface)->refs); }
static ULONG STDMETHODCALLTYPE fence_Release(ID3D12Fence *iface) { return InterlockedDecrement(&((struct fence *)iface)->refs); }
static UINT64 STDMETHODCALLTYPE fence_GetCompletedValue(ID3D12Fence *iface) { return ((struct fence *)iface)->completed; }
static HRESULT STDMETHODCALLTYPE fence_SetEventOnCompletion(ID3D12Fence *iface, UINT64 value, HANDLE event)
{
    struct fence *f = (struct fence *)iface;
    f->wait_value = value;
    f->wait_event = event;
    if ((UINT64)f->completed >= value)
        SetEvent(event);
    return S_OK;
}
static ID3D12FenceVtbl fence_vtbl;

struct pending { struct fence *fence; UINT64 value; };
static DWORD WINAPI complete_later(void *arg)
{
    struct pending *p = arg;
    Sleep(5);
    InterlockedExchange64(&p->fence->completed, p->value);
    if (p->fence->wait_event && p->fence->wait_value <= p->value)
        SetEvent(p->fence->wait_event);
    HeapFree(GetProcessHeap(), 0, p);
    return 0;
}

/* ---- queue */
struct queue { ID3D12CommandQueueVtbl *vtbl; LONG refs; };
static HRESULT STDMETHODCALLTYPE queue_QI(ID3D12CommandQueue *iface, REFIID riid, void **out) { *out = iface; return S_OK; }
static ULONG STDMETHODCALLTYPE queue_AddRef(ID3D12CommandQueue *iface) { return 2; }
static ULONG STDMETHODCALLTYPE queue_Release(ID3D12CommandQueue *iface) { return 1; }
static void STDMETHODCALLTYPE queue_ExecuteCommandLists(ID3D12CommandQueue *iface, UINT n, ID3D12CommandList *const *l)
{
    InterlockedIncrement(&g_executes);
}
static HRESULT STDMETHODCALLTYPE queue_Signal(ID3D12CommandQueue *iface, ID3D12Fence *fence, UINT64 value)
{
    struct pending *p = HeapAlloc(GetProcessHeap(), 0, sizeof(*p));
    InterlockedIncrement(&g_signals);
    p->fence = (struct fence *)fence;
    p->value = value;
    CloseHandle(CreateThread(NULL, 0, complete_later, p, 0, NULL));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE queue_Wait(ID3D12CommandQueue *iface, ID3D12Fence *fence, UINT64 value) { return S_OK; }
static ID3D12CommandQueueVtbl queue_vtbl;

/* ---- device */
struct device { ID3D12Device4Vtbl *vtbl; LONG refs; };

static HRESULT STDMETHODCALLTYPE dev_QI(ID3D12Device4 *iface, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_ID3D12Device) || IsEqualGUID(riid, &IID_ID3D12Device1)
            || IsEqualGUID(riid, &IID_ID3D12Device2) || IsEqualGUID(riid, &IID_ID3D12Device3)
            || IsEqualGUID(riid, &IID_ID3D12Device4) || IsEqualGUID(riid, &IID_IUnknown))
    {
        iface->lpVtbl->AddRef(iface);
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dev_AddRef(ID3D12Device4 *iface) { return InterlockedIncrement(&((struct device *)iface)->refs); }
static ULONG STDMETHODCALLTYPE dev_Release(ID3D12Device4 *iface) { return InterlockedDecrement(&((struct device *)iface)->refs); }
static HRESULT STDMETHODCALLTYPE dev_CreateComputePipelineState(ID3D12Device4 *iface,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    *out = new_object();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreatePipelineState(ID3D12Device4 *iface,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out)
{
    *out = new_object();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateCommandList(ID3D12Device4 *iface, UINT mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso, REFIID riid, void **out)
{
    struct list *l = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*l));
    l->vtbl = &list_vtbl;
    l->refs = 1;
    *out = l;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateGraphicsPipelineState(ID3D12Device4 *iface,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    *out = NULL;
    return E_INVALIDARG; /* pretend the translation layer can't compile it */
}
static HRESULT STDMETHODCALLTYPE dev_CheckFeatureSupport(ID3D12Device4 *iface, D3D12_FEATURE f, void *data, UINT size)
{
    memset(data, 0xab, size);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateCommandSignature(ID3D12Device4 *iface, const D3D12_COMMAND_SIGNATURE_DESC *d,
        ID3D12RootSignature *rs, REFIID riid, void **out)
{
    *out = new_object();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateRootSignature(ID3D12Device4 *iface, UINT mask, const void *blob, SIZE_T size,
        REFIID riid, void **out)
{
    *out = new_object();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateCommandQueue(ID3D12Device4 *iface, const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid, void **out)
{
    struct queue *q = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*q));
    q->vtbl = &queue_vtbl;
    *out = q;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateFence(ID3D12Device4 *iface, UINT64 initial, D3D12_FENCE_FLAGS flags,
        REFIID riid, void **out)
{
    struct fence *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*f));
    f->vtbl = &fence_vtbl;
    f->refs = 1;
    f->completed = initial;
    *out = f;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandList1(ID3D12Device4 *iface, UINT mask, D3D12_COMMAND_LIST_TYPE type,
        D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **out)
{
    return dev_CreateCommandList(iface, mask, type, NULL, NULL, riid, out);
}

/* Vtables live in a read-only section on real drivers; keep ours const-ish via a
 * separate page we protect, so the shim's VirtualProtect path is exercised. */
static ID3D12Device4Vtbl *dev_vtbl;
static struct device g_device;

HRESULT WINAPI mock_D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **out)
{
    DWORD old;

    if (!dev_vtbl)
    {
        dev_vtbl = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        dev_vtbl->QueryInterface = dev_QI;
        dev_vtbl->AddRef = dev_AddRef;
        dev_vtbl->Release = dev_Release;
        dev_vtbl->CreateComputePipelineState = dev_CreateComputePipelineState;
        dev_vtbl->CreatePipelineState = dev_CreatePipelineState;
        dev_vtbl->CreateCommandList = dev_CreateCommandList;
        dev_vtbl->CreateCommandList1 = dev_CreateCommandList1;
        dev_vtbl->CreateGraphicsPipelineState = dev_CreateGraphicsPipelineState;
        dev_vtbl->CheckFeatureSupport = dev_CheckFeatureSupport;
        dev_vtbl->CreateCommandSignature = dev_CreateCommandSignature;
        dev_vtbl->CreateRootSignature = dev_CreateRootSignature;
        dev_vtbl->CreateCommandQueue = dev_CreateCommandQueue;
        dev_vtbl->CreateFence = dev_CreateFence;
        VirtualProtect(dev_vtbl, 4096, PAGE_READONLY, &old);

        list_vtbl.QueryInterface = list_QI;
        list_vtbl.AddRef = list_AddRef;
        list_vtbl.Release = list_Release;
        list_vtbl.Reset = list_Reset;
        list_vtbl.ClearState = list_ClearState;
        list_vtbl.SetPipelineState = list_SetPipelineState;
        list_vtbl.Dispatch = list_Dispatch;
        list_vtbl.ResourceBarrier = list_ResourceBarrier;
        list_vtbl.ExecuteIndirect = list_ExecuteIndirect;

        fence_vtbl.QueryInterface = fence_QI;
        fence_vtbl.AddRef = fence_AddRef;
        fence_vtbl.Release = fence_Release;
        fence_vtbl.GetCompletedValue = fence_GetCompletedValue;
        fence_vtbl.SetEventOnCompletion = fence_SetEventOnCompletion;

        queue_vtbl.QueryInterface = queue_QI;
        queue_vtbl.AddRef = queue_AddRef;
        queue_vtbl.Release = queue_Release;
        queue_vtbl.ExecuteCommandLists = queue_ExecuteCommandLists;
        queue_vtbl.Signal = queue_Signal;
        queue_vtbl.Wait = queue_Wait;
    }
    g_device.vtbl = dev_vtbl;
    g_device.refs = 1;
    if (out)
        *out = &g_device;
    return S_OK;
}
