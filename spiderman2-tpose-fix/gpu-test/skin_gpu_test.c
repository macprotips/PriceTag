/* Runs the game's own CS_ModelSkinning compute shader through D3D12 (D3DMetal on a Mac)
 * on the synthetic inputs from skin_inputs.c and compares every output vertex with the
 * reference produced by skin_ref_vk (dxil-spirv + Mesa lavapipe, see make-reference.sh).
 *
 *   skin_gpu_test.exe <Spider-Man2.exe | shader.dxil> golden-skin.bin report.txt [more.dxil ...]
 *
 * The shader is read from the user's own game executable (found by its hash), so no game
 * code ships with this test. Extra .dxil arguments are run on the same inputs too (used
 * to try replacement shaders). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "skin_inputs.h"

#define SKINNING_HASH 0x3d50acdb571dce22ull

static FILE *report;

static void out(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (report)
    {
        va_start(ap, fmt);
        vfprintf(report, fmt, ap);
        va_end(ap);
        fflush(report);
    }
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < n; i++)
        h = (h * 0x100000001b3ull) ^ p[i];
    return h;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || !(data = malloc(n)) || fread(data, 1, n, f) != (size_t)n)
    {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = n;
    return data;
}

/* A .dxil file is used as is; anything else is searched for the DXBC container whose
 * hash is CS_ModelSkinning's (the game stores shaders as "IGSH" + 8 bytes + DXBC). */
static uint8_t *load_shader(const char *path, size_t *size)
{
    size_t n, i;
    uint8_t *data = read_file(path, &n);

    if (!data)
        return NULL;
    if (n > 32 && !memcmp(data, "DXBC", 4))
    {
        *size = n;
        return data;
    }
    for (i = 0; i + 32 < n; i++)
    {
        uint32_t len;

        if (memcmp(data + i, "DXBC", 4))
            continue;
        memcpy(&len, data + i + 24, 4);
        if (len < 32 || len > n - i)
            continue;
        if (fnv(data + i, len) == SKINNING_HASH)
        {
            uint8_t *copy = malloc(len);

            memcpy(copy, data + i, len);
            free(data);
            *size = len;
            return copy;
        }
    }
    free(data);
    return NULL;
}

static ID3D12Device *device;
static ID3D12CommandQueue *queue;
static ID3D12RootSignature *root_sig;

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = {0};
    ID3D12Resource *res = NULL;

    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
            &IID_ID3D12Resource, (void **)&res)))
        return NULL;
    return res;
}

static ID3D12Resource *upload(const void *data, UINT64 size)
{
    ID3D12Resource *res = make_buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *map;

    if (res && SUCCEEDED(ID3D12Resource_Map(res, 0, NULL, &map)))
    {
        memcpy(map, data, size);
        ID3D12Resource_Unmap(res, 0, NULL);
    }
    return res;
}

static int create_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE ranges[4] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 },      /* t0-t2 space0 */
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 97, 1, 3 },     /* t97 space1 */
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 101, 1, 4 },    /* t101 space1 */
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 5 },      /* u0 */
    };
    D3D12_ROOT_PARAMETER param = {0};
    D3D12_ROOT_SIGNATURE_DESC desc = {0};
    HRESULT (WINAPI *serialize)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;

    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 4;
    param.DescriptorTable.pDescriptorRanges = ranges;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    desc.NumParameters = 1;
    desc.pParameters = &param;
    serialize = (void *)GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12SerializeRootSignature");
    if (!serialize || FAILED(hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    {
        out("FAILED to serialize the root signature\n");
        return 0;
    }
    hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void **)&root_sig);
    if (FAILED(hr))
    {
        out("FAILED CreateRootSignature hr=%#lx\n", (unsigned long)hr);
        return 0;
    }
    return 1;
}

/* Runs one shader over the inputs; returns the output (SKIN_OUT_VERTS * 4 words) or NULL. */
static uint32_t *run_shader(const void *code, size_t code_size, const struct skin_inputs *in)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_RESOURCE_BARRIER barrier = {0};
    ID3D12PipelineState *pso = NULL;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12Fence *fence = NULL;
    ID3D12Resource *bones, *data, *entries, *raw, *inst, *output, *readback;
    UINT inc;
    HANDLE event;
    HRESULT hr;
    uint32_t *result = NULL;
    void *map;
    pd.pRootSignature = root_sig;
    pd.CS.pShaderBytecode = code;
    pd.CS.BytecodeLength = code_size;
    if (FAILED(hr = ID3D12Device_CreateComputePipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso)))
    {
        out("FAILED CreateComputePipelineState hr=%#lx\n", (unsigned long)hr);
        return NULL;
    }
    bones = upload(in->bones, sizeof(in->bones));
    data = upload(in->data, sizeof(in->data));
    entries = upload(in->entries, sizeof(in->entries));
    raw = upload(in->raw, sizeof(in->raw));
    inst = upload(in->inst, sizeof(in->inst));
    output = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)SKIN_OUT_VERTS * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    readback = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)SKIN_OUT_VERTS * 16, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    if (!bones || !data || !entries || !raw || !inst || !output || !readback)
    {
        out("FAILED to create buffers\n");
        return NULL;
    }

    if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap)))
        return out("FAILED CreateDescriptorHeap\n"), NULL;
    inc = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu);

#define SRV(res, fmt, n, stride, flags, slot) do { \
        memset(&sv, 0, sizeof(sv)); \
        sv.Format = fmt; sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; \
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; \
        sv.Buffer.NumElements = n; sv.Buffer.StructureByteStride = stride; sv.Buffer.Flags = flags; \
        { D3D12_CPU_DESCRIPTOR_HANDLE h_ = cpu; h_.ptr += (SIZE_T)(slot) * inc; \
          ID3D12Device_CreateShaderResourceView(device, res, &sv, h_); } } while (0)
    SRV(bones, DXGI_FORMAT_UNKNOWN, SKIN_BONES, 64, D3D12_BUFFER_SRV_FLAG_NONE, 0);
    SRV(data, DXGI_FORMAT_R32_UINT, SKIN_DATA_WORDS, 0, D3D12_BUFFER_SRV_FLAG_NONE, 1);
    SRV(entries, DXGI_FORMAT_UNKNOWN, SKIN_GROUPS, 32, D3D12_BUFFER_SRV_FLAG_NONE, 2);
    SRV(raw, DXGI_FORMAT_R32_TYPELESS, SKIN_RAW_WORDS, 0, D3D12_BUFFER_SRV_FLAG_RAW, 3);
    SRV(inst, DXGI_FORMAT_UNKNOWN, SKIN_INST_RECORDS, 64, D3D12_BUFFER_SRV_FLAG_NONE, 4);
#undef SRV
    uv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = SKIN_OUT_VERTS;
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;

        h.ptr += 5 * (SIZE_T)inc;
        ID3D12Device_CreateUnorderedAccessView(device, output, NULL, &uv, h);
    }

    if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc))
            || FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        return out("FAILED to create the command list\n"), NULL;
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;

        heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
        ID3D12GraphicsCommandList_SetComputeRootSignature(list, root_sig);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    }
    ID3D12GraphicsCommandList_Dispatch(list, SKIN_GROUPS, 1, 1);
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyResource(list, readback, output);
    if (FAILED(ID3D12GraphicsCommandList_Close(list)))
        return out("FAILED to close the command list\n"), NULL;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    event = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, 1, event);
    if (WaitForSingleObject(event, 60000) != WAIT_OBJECT_0)
        return out("FAILED: the GPU didn't finish within 60 s\n"), NULL;

    if (SUCCEEDED(ID3D12Resource_Map(readback, 0, NULL, &map)))
    {
        result = malloc((size_t)SKIN_OUT_VERTS * 16);
        memcpy(result, map, (size_t)SKIN_OUT_VERTS * 16);
        ID3D12Resource_Unmap(readback, 0, NULL);
    }
    return result;
}

static int s16(uint32_t w, int hi)
{
    return (int)(int16_t)(hi ? w >> 16 : w & 0xffff);
}

/* Compares one run with the reference: exact, close (rounding: each 16-bit position
 * component within 2 and each 10-bit normal component within 2), or wrong. Wrong
 * vertices are split by the influence count of their 32-vertex block. */
static int compare(const char *label, const uint32_t *got, const uint32_t *ref, const struct skin_inputs *in)
{
    unsigned int exact = 0, close = 0, wrong = 0, missing = 0, extra = 0, shown = 0;
    unsigned int by_infl_total[17] = {0}, by_infl_wrong[17] = {0}, g, v;

    for (g = 0; g < SKIN_GROUPS; g++)
    {
        const uint32_t *e = in->entries + g * 8;
        uint32_t count = e[6] & 4095, hdr = e[2] & 0x3fffffff;

        for (v = 0; v < SKIN_MAX_VERTS; v++)
        {
            uint32_t idx = e[1] + v;
            const uint32_t *a = got + idx * 4, *b = ref + idx * 4;
            uint32_t header = v < count ? in->data[hdr + v / 32] : 0;
            unsigned int infl = (e[2] >> 30) ? 16 : ((header >> 21) & 15) + 1;
            int ok_close = 1, k;

            if (!(b[0] | b[1] | b[2] | b[3]))
            {
                if (a[0] | a[1] | a[2] | a[3])
                    extra++;
                continue;
            }
            if (v < count)
                by_infl_total[infl]++;
            if (!memcmp(a, b, 16))
            {
                exact++;
                continue;
            }
            if (!(a[0] | a[1] | a[2] | a[3]))
                missing++;
            for (k = 0; k < 2 && ok_close; k++)
                ok_close = abs(s16(a[0], k) - s16(b[0], k)) <= 2;
            ok_close = ok_close && abs(s16(a[1], 0) - s16(b[1], 0)) <= 2 && (a[1] >> 16) == (b[1] >> 16);
            for (k = 0; k < 3 && ok_close; k++)
                ok_close = abs((int)((a[2] >> (10 * k)) & 1023) - (int)((b[2] >> (10 * k)) & 1023)) <= 2;
            ok_close = ok_close && (a[2] >> 30) == (b[2] >> 30) && a[3] == b[3];
            if (ok_close)
            {
                close++;
                continue;
            }
            wrong++;
            if (v < count)
                by_infl_wrong[infl]++;
            if (shown++ < 12)
                out("    vertex %u (group %u, mode %u, block influences %u, alt %u): got %08x %08x %08x %08x  want %08x %08x %08x %08x\n",
                        idx, g, e[2] >> 30, infl, (header >> 20) & 1, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
    }
    out("%s: %u exact, %u within rounding, %u WRONG (%u of them left empty), %u written where the reference wrote nothing\n",
            label, exact, close, wrong, missing, extra);
    out("  wrong by influences per vertex:");
    for (v = 1; v <= 16; v++)
        if (by_infl_total[v])
            out(" %u:%u/%u", v, by_infl_wrong[v], by_infl_total[v]);
    out("\n");
    return wrong == 0 && extra == 0;
}

int main(int argc, char **argv)
{
    static struct skin_inputs in;
    HRESULT (WINAPI *create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    uint8_t *shader;
    uint32_t *ref, *first = NULL;
    size_t shader_size, ref_size;
    HMODULE d3d12;
    int i, all_ok = 1;

    if (argc < 4)
    {
        printf("usage: %s <Spider-Man2.exe|shader.dxil> golden-skin.bin report.txt [more.dxil ...]\n", argv[0]);
        return 2;
    }
    report = fopen(argv[3], "w");
    out("Spider-Man 2 skinning shader GPU check\n");
    if (!(shader = load_shader(argv[1], &shader_size)))
    {
        out("FAILED: couldn't find the skinning shader (hash %016llx) in %s\n", (unsigned long long)SKINNING_HASH, argv[1]);
        return 1;
    }
    out("shader: %zu bytes from %s (hash %016llx)\n", shader_size, argv[1], (unsigned long long)fnv(shader, shader_size));
    if (!(ref = (uint32_t *)read_file(argv[2], &ref_size)) || ref_size != (size_t)SKIN_OUT_VERTS * 16)
    {
        out("FAILED: can't read the reference %s\n", argv[2]);
        return 1;
    }
    skin_make_inputs(&in, 0);

    if (!(d3d12 = LoadLibraryA("d3d12.dll"))
            || !(create_device = (void *)GetProcAddress(d3d12, "D3D12CreateDevice"))
            || FAILED(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
    {
        out("FAILED: no D3D12 device\n");
        return 1;
    }
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue)) || !create_root_signature())
        return 1;

    /* The game's shader, twice (a difference between the two runs means a race). */
    for (i = 0; i < 2; i++)
    {
        uint32_t *got = run_shader(shader, shader_size, &in);

        if (!got)
            return 1;
        all_ok &= compare(i ? "game shader, run 2" : "game shader, run 1", got, ref, &in);
        if (!first)
            first = got;
        else
            out("run 1 and run 2 %s\n", memcmp(first, got, (size_t)SKIN_OUT_VERTS * 16) ? "DIFFER (non-deterministic)" : "are identical");
    }
    out("RESULT: %s\n", all_ok ? "the game's skinning shader matches the reference on this GPU"
                               : "the game's skinning shader gives WRONG results on this GPU");
    /* Candidate replacements. */
    for (i = 4; i < argc; i++)
    {
        size_t n;
        uint8_t *alt = read_file(argv[i], &n);
        uint32_t *got;

        if (!alt)
        {
            out("%s: can't read\n", argv[i]);
            continue;
        }
        if ((got = run_shader(alt, n, &in)))
            out("RESULT (%s): %s\n", argv[i], compare(argv[i], got, ref, &in) ? "matches the reference" : "WRONG");
        else
            out("RESULT (%s): could not run\n", argv[i]);
    }
    if (report)
        fclose(report);
    return 0;
}
