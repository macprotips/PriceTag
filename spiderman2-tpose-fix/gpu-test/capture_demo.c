/* Self-test for sm2fix's dispatch capture and replay.exe: runs the game's
 * CS_ZivaTensorSkinning (read from Spider-Man2.exe) once through d3d12.dll (the shim, with
 * capture on) on synthetic but in-range inputs, laid out like the game does it (typed views
 * at offsets into shared buffers, quantized weights as R8_SINT). The shim writes
 * sm2fix-capture\<hash>-1.bin; replay.exe on the same driver must then report a match.
 *
 *   capture_demo.exe <Spider-Man2.exe> */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shim/sm2fix_core.h"

#define TENSOR_HASH 0xbd8cf9af848f2800ull
#define THREADS 192

static ID3D12Device *device;

static ID3D12Resource *buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = {0};
    ID3D12Resource *res = NULL;

    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource,
            (void **)&res);
    return res;
}

int main(int argc, char **argv)
{
    HRESULT (WINAPI *create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    HRESULT (WINAPI *serialize)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 13, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 13 },
    };
    D3D12_ROOT_PARAMETER param = {0};
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 14, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    struct { UINT slot; DXGI_FORMAT format; UINT64 first; UINT num; } views[6] = {
        { 0, DXGI_FORMAT_R32_FLOAT, 0, 1024 },      /* t0: inputs */
        { 1, DXGI_FORMAT_R32_UINT, 1100, THREADS },    /* t1: record per thread */
        { 2, DXGI_FORMAT_R32_UINT, 1300, 20 },         /* t2: records (5 words) */
        { 3, DXGI_FORMAT_R8_SINT, 7000, 1024 },       /* t3: quantized weights */
        { 11, DXGI_FORMAT_R32_FLOAT, 1340, 64 },      /* t11: row scales */
        { 12, DXGI_FORMAT_R32_SINT, 1420, 1 },        /* t12: thread count */
    };
    static uint8_t data[8192];
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    ID3D12CommandQueue *queue;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *shared, *out;
    ID3D12Fence *fence;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    uint8_t *exe = NULL, *shader = NULL;
    size_t exe_size = 0, shader_size = 0, i;
    HANDLE event;
    HMODULE d3d12;
    UINT inc, k;
    FILE *f;
    void *map;

    if (argc < 2 || !(f = fopen(argv[1], "rb")))
        return printf("usage: %s Spider-Man2.exe\n", argv[0]), 2;
    fseek(f, 0, SEEK_END);
    exe_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    exe = malloc(exe_size);
    if (fread(exe, 1, exe_size, f) != exe_size)
        return 1;
    fclose(f);
    for (i = 0; i + 32 < exe_size && !shader; i++)
    {
        uint32_t len;

        if (exe[i] != 'D' || memcmp(exe + i, "DXBC", 4))
            continue;
        memcpy(&len, exe + i + 24, 4);
        if (len >= 32 && len <= exe_size - i && sm2_shader_hash(exe + i, len) == TENSOR_HASH)
        {
            shader = exe + i;
            shader_size = len;
        }
    }
    if (!shader)
        return printf("FAIL: CS_ZivaTensorSkinning not found\n"), 1;

    /* inputs, at each view's byte offset in one shared buffer */
    {
        float *in = (float *)data, *scale = (float *)(data + 1340 * 4);
        uint32_t *rec_of = (uint32_t *)(data + 1100 * 4), *rec = (uint32_t *)(data + 1300 * 4);
        int8_t *w = (int8_t *)(data + 7000);
        int32_t *count = (int32_t *)(data + 1420 * 4);

        for (k = 0; k < 1024; k++)
        {
            in[k] = (float)((int)(k * 37 % 101) - 50) / 16.0f;
            w[k] = (int8_t)(k * 11 % 255 - 127);
        }
        for (k = 0; k < THREADS; k++)
            rec_of[k] = k % 4;
        for (k = 0; k < 4; k++)
        {
            rec[k * 5 + 1] = 3 + k;     /* terms per output */
            rec[k * 5 + 2] = 7 * k;     /* weight base */
            rec[k * 5 + 3] = 0;         /* row base */
            rec[k * 5 + 4] = 5 * k;     /* input base */
        }
        for (k = 0; k < 64; k++)
            scale[k] = 0.25f + k / 128.0f;
        *count = THREADS;
    }

    d3d12 = LoadLibraryA("d3d12.dll");
    create_device = (void *)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!create_device || FAILED(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
        return printf("FAIL: no device\n"), 1;
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    rsd.NumParameters = 1;
    rsd.pParameters = &param;
    serialize = (void *)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
    if (!serialize || FAILED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))
            || FAILED(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
                    ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs)))
        return printf("FAIL: root signature\n"), 1;
    pd.pRootSignature = rs;
    pd.CS.pShaderBytecode = shader;
    pd.CS.BytecodeLength = shader_size;
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateComputePipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso))
            || FAILED(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue))
            || FAILED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap))
            || FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator, (void **)&alloc))
            || FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        return printf("FAIL: setup\n"), 1;

    shared = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(data), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    out = buffer(D3D12_HEAP_TYPE_DEFAULT, 4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!shared || !out || FAILED(ID3D12Resource_Map(shared, 0, NULL, &map)))
        return printf("FAIL: buffers\n"), 1;
    memcpy(map, data, sizeof(data));
    ID3D12Resource_Unmap(shared, 0, NULL);

    inc = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu);
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu);
    for (k = 0; k < 13; k++)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
        D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu.ptr + (SIZE_T)k * inc };

        sv.Format = DXGI_FORMAT_R32_UINT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ID3D12Device_CreateShaderResourceView(device, NULL, &sv, h);
    }
    for (k = 0; k < 6; k++)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
        D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu.ptr + (SIZE_T)views[k].slot * inc };

        sv.Format = views[k].format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.FirstElement = views[k].first;
        sv.Buffer.NumElements = views[k].num;
        ID3D12Device_CreateShaderResourceView(device, shared, &sv, h);
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {0};
        D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu.ptr + 13 * (SIZE_T)inc };

        uv.Format = DXGI_FORMAT_R32_FLOAT;
        uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.FirstElement = 64;
        uv.Buffer.NumElements = THREADS;
        ID3D12Device_CreateUnorderedAccessView(device, out, NULL, &uv, h);
    }
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, THREADS / 64, 1, 1);
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    event = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, 1, event);
    if (WaitForSingleObject(event, 60000) != WAIT_OBJECT_0)
        return printf("FAIL: GPU timeout\n"), 1;
    printf("capture_demo: dispatched CS_ZivaTensorSkinning (%u threads)\n", THREADS);
    return 0;
}
