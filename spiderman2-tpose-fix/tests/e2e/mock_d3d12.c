/* A stand-in for the translation layer's d3d12.dll (D3DMetal, vkd3d...). Implements just
 * enough of ID3D12Device / ID3D12GraphicsCommandList for the shim to hook, and records
 * every ResourceBarrier so the test can check what the shim injected. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

static LONG g_uav_barriers, g_other_barriers, g_dispatches;

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

static ID3D12GraphicsCommandListVtbl list_vtbl;

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
        VirtualProtect(dev_vtbl, 4096, PAGE_READONLY, &old);

        list_vtbl.QueryInterface = list_QI;
        list_vtbl.AddRef = list_AddRef;
        list_vtbl.Release = list_Release;
        list_vtbl.Reset = list_Reset;
        list_vtbl.ClearState = list_ClearState;
        list_vtbl.SetPipelineState = list_SetPipelineState;
        list_vtbl.Dispatch = list_Dispatch;
        list_vtbl.ResourceBarrier = list_ResourceBarrier;
    }
    g_device.vtbl = dev_vtbl;
    g_device.refs = 1;
    if (out)
        *out = &g_device;
    return S_OK;
}
