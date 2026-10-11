/* Checks how the D3D12 driver (D3DMetal on a Mac) decodes typed buffer views: for each
 * buffer format, a tiny shader copies Buffer<T>[i] into a 4x32-bit RWBuffer, and every
 * element is compared with the exact value the D3D12 spec defines for that format.
 *
 * Why: the game's Ziva face/cloth deformation (CS_ZivaTensorSkinning etc.) reads its
 * quantized weights through integer typed buffers. If the driver decodes such formats
 * wrongly on M1-M4, faces and cloth would end up undeformed or collapsed.
 *
 *   format_test.exe <dir with fmt_float.dxil, fmt_int.dxil, fmt_uint.dxil> report.txt */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 256
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

enum kind { K_UNORM, K_SNORM, K_SINT, K_UINT, K_FLOAT, K_RGB10A2_UNORM, K_RGB10A2_UINT, K_R11G11B10, K_BGRA8_UNORM };
enum shader { SH_FLOAT, SH_INT, SH_UINT };

struct format
{
    const char *name;
    DXGI_FORMAT dxgi;
    enum kind kind;
    int channels, bits;     /* bits per channel (ignored for packed kinds) */
    enum shader shader;
};

static const struct format formats[] = {
    { "R8_UNORM", DXGI_FORMAT_R8_UNORM, K_UNORM, 1, 8, SH_FLOAT },
    { "R8_SNORM", DXGI_FORMAT_R8_SNORM, K_SNORM, 1, 8, SH_FLOAT },
    { "R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM, K_UNORM, 2, 8, SH_FLOAT },
    { "R8G8_SNORM", DXGI_FORMAT_R8G8_SNORM, K_SNORM, 2, 8, SH_FLOAT },
    { "R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, K_UNORM, 4, 8, SH_FLOAT },
    { "R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_SNORM, K_SNORM, 4, 8, SH_FLOAT },
    { "B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM, K_BGRA8_UNORM, 4, 8, SH_FLOAT },
    { "R16_UNORM", DXGI_FORMAT_R16_UNORM, K_UNORM, 1, 16, SH_FLOAT },
    { "R16_SNORM", DXGI_FORMAT_R16_SNORM, K_SNORM, 1, 16, SH_FLOAT },
    { "R16G16_UNORM", DXGI_FORMAT_R16G16_UNORM, K_UNORM, 2, 16, SH_FLOAT },
    { "R16G16_SNORM", DXGI_FORMAT_R16G16_SNORM, K_SNORM, 2, 16, SH_FLOAT },
    { "R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM, K_UNORM, 4, 16, SH_FLOAT },
    { "R16G16B16A16_SNORM", DXGI_FORMAT_R16G16B16A16_SNORM, K_SNORM, 4, 16, SH_FLOAT },
    { "R16_FLOAT", DXGI_FORMAT_R16_FLOAT, K_FLOAT, 1, 16, SH_FLOAT },
    { "R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT, K_FLOAT, 2, 16, SH_FLOAT },
    { "R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, K_FLOAT, 4, 16, SH_FLOAT },
    { "R32_FLOAT", DXGI_FORMAT_R32_FLOAT, K_FLOAT, 1, 32, SH_FLOAT },
    { "R32G32_FLOAT", DXGI_FORMAT_R32G32_FLOAT, K_FLOAT, 2, 32, SH_FLOAT },
    { "R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT, K_FLOAT, 4, 32, SH_FLOAT },
    { "R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM, K_RGB10A2_UNORM, 4, 0, SH_FLOAT },
    { "R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT, K_R11G11B10, 3, 0, SH_FLOAT },
    { "R8_SINT", DXGI_FORMAT_R8_SINT, K_SINT, 1, 8, SH_INT },
    { "R8G8_SINT", DXGI_FORMAT_R8G8_SINT, K_SINT, 2, 8, SH_INT },
    { "R8G8B8A8_SINT", DXGI_FORMAT_R8G8B8A8_SINT, K_SINT, 4, 8, SH_INT },
    { "R16_SINT", DXGI_FORMAT_R16_SINT, K_SINT, 1, 16, SH_INT },
    { "R16G16_SINT", DXGI_FORMAT_R16G16_SINT, K_SINT, 2, 16, SH_INT },
    { "R16G16B16A16_SINT", DXGI_FORMAT_R16G16B16A16_SINT, K_SINT, 4, 16, SH_INT },
    { "R32_SINT", DXGI_FORMAT_R32_SINT, K_SINT, 1, 32, SH_INT },
    { "R32G32_SINT", DXGI_FORMAT_R32G32_SINT, K_SINT, 2, 32, SH_INT },
    { "R32G32B32A32_SINT", DXGI_FORMAT_R32G32B32A32_SINT, K_SINT, 4, 32, SH_INT },
    { "R8_UINT", DXGI_FORMAT_R8_UINT, K_UINT, 1, 8, SH_UINT },
    { "R8G8_UINT", DXGI_FORMAT_R8G8_UINT, K_UINT, 2, 8, SH_UINT },
    { "R8G8B8A8_UINT", DXGI_FORMAT_R8G8B8A8_UINT, K_UINT, 4, 8, SH_UINT },
    { "R16_UINT", DXGI_FORMAT_R16_UINT, K_UINT, 1, 16, SH_UINT },
    { "R16G16_UINT", DXGI_FORMAT_R16G16_UINT, K_UINT, 2, 16, SH_UINT },
    { "R16G16B16A16_UINT", DXGI_FORMAT_R16G16B16A16_UINT, K_UINT, 4, 16, SH_UINT },
    { "R32_UINT", DXGI_FORMAT_R32_UINT, K_UINT, 1, 32, SH_UINT },
    { "R32G32_UINT", DXGI_FORMAT_R32G32_UINT, K_UINT, 2, 32, SH_UINT },
    { "R32G32B32A32_UINT", DXGI_FORMAT_R32G32B32A32_UINT, K_UINT, 4, 32, SH_UINT },
    { "R10G10B10A2_UINT", DXGI_FORMAT_R10G10B10A2_UINT, K_RGB10A2_UINT, 4, 0, SH_UINT },
};

static int bytes_per_element(const struct format *f)
{
    return f->bits ? f->channels * f->bits / 8 : 4;
}

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static float half_to_float(uint16_t h)
{
    unsigned int e = (h >> 10) & 31, m = h & 1023, s = h >> 15;
    float v;

    if (!e)
        v = ldexpf((float)m, -24);
    else if (e == 31)
        v = m ? NAN : INFINITY;
    else
        v = ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

static float small_float(unsigned int v, int mbits)
{
    unsigned int e = v >> mbits, m = v & ((1u << mbits) - 1);

    if (!e)
        return ldexpf((float)m, -14 - mbits);
    if (e == 31)
        return m ? NAN : INFINITY;
    return ldexpf((float)(m | (1u << mbits)), (int)e - 15 - mbits);
}

/* The four 32-bit values the shader should see for element i (as float or int bits). */
static void expected(const struct format *f, const uint8_t *el, uint32_t want[4])
{
    float fv[4] = {0, 0, 0, 1};
    int32_t iv[4] = {0, 0, 0, 1};
    int c;

    for (c = 0; c < f->channels && f->bits; c++)
    {
        uint32_t raw = 0;

        memcpy(&raw, el + c * f->bits / 8, f->bits / 8);
        switch (f->kind)
        {
        case K_UNORM: case K_BGRA8_UNORM:
            fv[c] = (float)raw / (float)((f->bits == 32 ? 0 : (1u << f->bits)) - 1);
            break;
        case K_SNORM:
        {
            int32_t s = f->bits == 8 ? (int8_t)raw : (int16_t)raw;
            float m = (float)((1 << (f->bits - 1)) - 1);

            fv[c] = s / m < -1.0f ? -1.0f : s / m;
            break;
        }
        case K_FLOAT:
            if (f->bits == 16)
                fv[c] = half_to_float((uint16_t)raw);
            else
                memcpy(&fv[c], &raw, 4);
            break;
        case K_SINT:
            iv[c] = f->bits == 8 ? (int8_t)raw : f->bits == 16 ? (int16_t)raw : (int32_t)raw;
            break;
        case K_UINT:
            iv[c] = (int32_t)raw;
            break;
        default:
            break;
        }
    }
    if (f->kind == K_BGRA8_UNORM)
    {
        float t = fv[0];

        fv[0] = fv[2];
        fv[2] = t;
    }
    if (f->kind == K_RGB10A2_UNORM || f->kind == K_RGB10A2_UINT || f->kind == K_R11G11B10)
    {
        uint32_t v;

        memcpy(&v, el, 4);
        if (f->kind == K_RGB10A2_UNORM)
        {
            fv[0] = (v & 1023) / 1023.0f;
            fv[1] = ((v >> 10) & 1023) / 1023.0f;
            fv[2] = ((v >> 20) & 1023) / 1023.0f;
            fv[3] = (v >> 30) / 3.0f;
        }
        else if (f->kind == K_RGB10A2_UINT)
        {
            iv[0] = v & 1023;
            iv[1] = (v >> 10) & 1023;
            iv[2] = (v >> 20) & 1023;
            iv[3] = v >> 30;
        }
        else
        {
            fv[0] = small_float(v & 2047, 6);
            fv[1] = small_float((v >> 11) & 2047, 6);
            fv[2] = small_float(v >> 22, 5);
            fv[3] = 1.0f;
        }
    }
    for (c = 0; c < 4; c++)
    {
        if (f->shader == SH_FLOAT)
            memcpy(&want[c], &fv[c], 4);
        else
            want[c] = (uint32_t)iv[c];
    }
}

/* Source bytes: deterministic, covering 0, extremes and sign bits; no NaN/Inf for floats. */
static void make_source(const struct format *f, uint8_t *data)
{
    int i, bpe = bytes_per_element(f);

    for (i = 0; i < N * bpe; i++)
    {
        uint32_t r = rnd();
        static const uint8_t special[] = {0x00, 0x01, 0x7f, 0x80, 0x81, 0xfe, 0xff};

        data[i] = (i / bpe) < 8 ? special[(i / bpe + i) % 7] : (uint8_t)r;
    }
    if (f->kind == K_FLOAT && f->bits == 16)
        for (i = 0; i < N * bpe; i += 2)
            if (((data[i + 1] >> 2) & 31) == 31)
                data[i + 1] &= ~0x40;   /* clear one exponent bit: no Inf/NaN */
    if (f->kind == K_FLOAT && f->bits == 32)
        for (i = 0; i < N * bpe; i += 4)
            if (((data[i + 3] & 0x7f) == 0x7f) && (data[i + 2] & 0x80))
                data[i + 3] &= ~0x40;
    if (f->kind == K_R11G11B10)
        for (i = 0; i < N * bpe; i += 4)
        {
            uint32_t v;

            memcpy(&v, data + i, 4);
            if (((v >> 6) & 31) == 31) v &= ~(1u << 10);
            if (((v >> 17) & 31) == 31) v &= ~(1u << 21);
            if (((v >> 27) & 31) == 31) v &= ~(1u << 31);
            memcpy(data + i, &v, 4);
        }
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
    data = malloc(n);
    if (fread(data, 1, n, f) != (size_t)n)
        n = 0;
    fclose(f);
    *size = n;
    return n ? data : NULL;
}

/* Runs one format; returns the number of wrong elements, or -1 if it couldn't run. */
static int run_format(const struct format *f, ID3D12PipelineState *pso)
{
    static const DXGI_FORMAT out_formats[] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_SINT,
            DXGI_FORMAT_R32G32B32A32_UINT };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    D3D12_RESOURCE_BARRIER barrier = {0};
    ID3D12DescriptorHeap *heap;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    ID3D12Resource *src, *dst, *rb;
    uint8_t data[N * 16];
    uint32_t *got;
    void *map;
    HANDLE event;
    UINT inc;
    int i, wrong = 0, shown = 0, bpe = bytes_per_element(f);

    make_source(f, data);
    src = make_buffer(D3D12_HEAP_TYPE_UPLOAD, N * bpe, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    dst = make_buffer(D3D12_HEAP_TYPE_DEFAULT, N * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, N * 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!src || !dst || !rb || FAILED(ID3D12Resource_Map(src, 0, NULL, &map)))
        return -1;
    memcpy(map, data, N * bpe);
    ID3D12Resource_Unmap(src, 0, NULL);

    if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap)))
        return -1;
    inc = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu);
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu);
    sv.Format = f->dxgi;
    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Buffer.NumElements = N;
    ID3D12Device_CreateShaderResourceView(device, src, &sv, cpu);
    uv.Format = out_formats[f->shader];
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = N;
    cpu.ptr += inc;
    ID3D12Device_CreateUnorderedAccessView(device, dst, NULL, &uv, cpu);

    if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc))
            || FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso,
                    &IID_ID3D12GraphicsCommandList, (void **)&list))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        return -1;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root_sig);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, N / 64, 1, 1);
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyResource(list, rb, dst);
    if (FAILED(ID3D12GraphicsCommandList_Close(list)))
        return -1;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    event = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, 1, event);
    if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0 || FAILED(ID3D12Resource_Map(rb, 0, NULL, &map)))
        return -1;
    got = map;
    for (i = 0; i < N; i++)
    {
        uint32_t want[4];
        int c, bad = 0;

        expected(f, data + i * bpe, want);
        for (c = 0; c < 4; c++)
        {
            if (f->shader == SH_FLOAT)
            {
                float a, b;

                memcpy(&a, &got[i * 4 + c], 4);
                memcpy(&b, &want[c], 4);
                if (!(a == b || (isnan(a) && isnan(b)) || fabsf(a - b) <= 1e-6f * fabsf(b) + 1e-37f))
                    bad = 1;
            }
            else if (got[i * 4 + c] != want[c])
                bad = 1;
        }
        if (bad)
        {
            wrong++;
            if (shown++ < 3)
            {
                if (f->shader == SH_FLOAT)
                    out("    element %d: got (%g %g %g %g) want (%g %g %g %g)\n", i, *(float *)&got[i * 4], *(float *)&got[i * 4 + 1],
                            *(float *)&got[i * 4 + 2], *(float *)&got[i * 4 + 3], *(float *)&want[0], *(float *)&want[1],
                            *(float *)&want[2], *(float *)&want[3]);
                else
                    out("    element %d: got (%d %d %d %d) want (%d %d %d %d)\n", i, (int)got[i * 4], (int)got[i * 4 + 1],
                            (int)got[i * 4 + 2], (int)got[i * 4 + 3], (int)want[0], (int)want[1], (int)want[2], (int)want[3]);
            }
        }
    }
    ID3D12Resource_Unmap(rb, 0, NULL);
    return wrong;
}

int main(int argc, char **argv)
{
    static const char *names[] = { "fmt_float.dxil", "fmt_int.dxil", "fmt_uint.dxil" };
    HRESULT (WINAPI *create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    HRESULT (WINAPI *serialize)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 } };
    D3D12_ROOT_PARAMETER param = {0};
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    ID3D12PipelineState *psos[3] = {0};
    ID3DBlob *blob, *err;
    HMODULE d3d12;
    unsigned int i, bad_formats = 0;

    if (argc < 3)
    {
        printf("usage: %s <shader dir> report.txt\n", argv[0]);
        return 2;
    }
    report = fopen(argv[2], "w");
    out("Typed buffer format check (%u formats x %d elements)\n", (unsigned)(sizeof(formats) / sizeof(formats[0])), N);
    if (!(d3d12 = LoadLibraryA("d3d12.dll")) || !(create_device = (void *)GetProcAddress(d3d12, "D3D12CreateDevice"))
            || FAILED(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
        return out("FAILED: no D3D12 device\n"), 1;
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    rsd.NumParameters = 1;
    rsd.pParameters = &param;
    serialize = (void *)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
    if (!serialize || FAILED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))
            || FAILED(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
                    ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root_sig)))
        return out("FAILED: root signature\n"), 1;
    for (i = 0; i < 3; i++)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
        char path[MAX_PATH];
        size_t n;
        uint8_t *code;

        snprintf(path, sizeof(path), "%s\\%s", argv[1], names[i]);
        if (!(code = read_file(path, &n)))
            return out("FAILED: can't read %s\n", path), 1;
        pd.pRootSignature = root_sig;
        pd.CS.pShaderBytecode = code;
        pd.CS.BytecodeLength = n;
        if (FAILED(ID3D12Device_CreateComputePipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&psos[i])))
            return out("FAILED: pipeline %s\n", names[i]), 1;
    }
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
    {
        int wrong = run_format(&formats[i], psos[formats[i].shader]);

        if (wrong)
            bad_formats++;
        out("%-20s %s", formats[i].name, wrong < 0 ? "COULD NOT RUN\n" : wrong ? "" : "ok\n");
        if (wrong > 0)
            out("WRONG: %d of %d elements\n", wrong, N);
    }
    out("RESULT: %u of %u formats decoded wrongly\n", bad_formats, (unsigned)(sizeof(formats) / sizeof(formats[0])));
    if (report)
        fclose(report);
    return 0;
}
