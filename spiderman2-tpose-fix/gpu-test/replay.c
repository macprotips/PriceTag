/* Re-runs a compute dispatch captured by sm2fix (capture = N, files in sm2fix-capture\) on
 * this machine's D3D12 driver and compares every UAV with what the GPU that made the
 * capture wrote. Run on a reference driver (vkd3d-proton + lavapipe), a mismatch means the
 * capturing driver (D3DMetal on the Mac) computed that shader wrongly.
 *
 *   replay.exe <Spider-Man2.exe> report.txt capture.bin [capture.bin ...]
 *
 * The shader is read from the game executable by the hash stored in the capture. */
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
#include "../shim/sm2fix_core.h"

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

/* The game stores shaders as DXBC containers; find the one with this hash. */
static uint8_t *find_shader(const uint8_t *exe, size_t n, uint64_t hash, size_t *size)
{
    size_t i;

    for (i = 0; i + 32 < n; i++)
    {
        uint32_t len;

        if (exe[i] != 'D' || memcmp(exe + i, "DXBC", 4))
            continue;
        memcpy(&len, exe + i + 24, 4);
        if (len < 32 || len > n - i)
            continue;
        if (sm2_shader_hash(exe + i, len) == hash)
        {
            uint8_t *copy = malloc(len);

            memcpy(copy, exe + i, len);
            *size = len;
            return copy;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- capture file */

#define ROOT_PARAMS 64
struct view
{
    uint32_t param, slot, kind, dim, format, num, stride, flags, heap_type, has_after;
    uint64_t first, res_width, byte_off, byte_len, res_id, va;
    const uint8_t *before, *after;
    int group;
};

struct capture
{
    uint64_t hash;
    uint32_t x, y, z, rs_size, nviews;
    const uint8_t *rs_blob;
    uint64_t root_val[ROOT_PARAMS];
    uint32_t consts[ROOT_PARAMS][32];
    struct view *v;
};

static int parse_capture(const uint8_t *p, size_t n, struct capture *c)
{
    const uint8_t *end = p + n;
    uint32_t i, nparams, version, nconst;

#define NEED(k) do { if ((size_t)(end - p) < (size_t)(k)) return -1; } while (0)
#define R32(dst) do { NEED(4); memcpy(&(dst), p, 4); p += 4; } while (0)
#define R64(dst) do { NEED(8); memcpy(&(dst), p, 8); p += 8; } while (0)
    memset(c, 0, sizeof(*c));
    NEED(8);
    if (memcmp(p, "SM2C", 4))
        return -1;
    memcpy(&version, p + 4, 4);
    if (version < 1 || version > 2)
        return -1;
    nconst = version == 1 ? 16 : 32;
    p += 8;
    R64(c->hash);
    R32(c->x); R32(c->y); R32(c->z);
    R32(c->rs_size);
    NEED(c->rs_size);
    c->rs_blob = p;
    p += c->rs_size;
    R32(nparams);
    if (nparams > ROOT_PARAMS)
        return -1;
    for (i = 0; i < nparams; i++)
    {
        R64(c->root_val[i]);
        NEED(nconst * 4);
        memcpy(c->consts[i], p, nconst * 4);
        p += nconst * 4;
    }
    R32(c->nviews);
    if (c->nviews > 4096 || !(c->v = calloc(c->nviews + 1, sizeof(*c->v))))
        return -1;
    for (i = 0; i < c->nviews; i++)
    {
        struct view *v = &c->v[i];

        R32(v->param); R32(v->slot); R32(v->kind); R32(v->dim); R32(v->format); R32(v->num); R32(v->stride);
        R32(v->flags); R32(v->heap_type); R32(v->has_after);
        R64(v->first); R64(v->res_width); R64(v->byte_off); R64(v->byte_len); R64(v->res_id); R64(v->va);
    }
    for (i = 0; i < c->nviews; i++)
        if (c->v[i].byte_len)
        {
            NEED(c->v[i].byte_len);
            c->v[i].before = p;
            p += c->v[i].byte_len;
        }
    for (i = 0; i < c->nviews; i++)
        if (c->v[i].has_after)
        {
            NEED(c->v[i].byte_len);
            c->v[i].after = p;
            p += c->v[i].byte_len;
        }
#undef NEED
#undef R32
#undef R64
    return 0;
}

/* ------------------------------------------------------------------ device */

static ID3D12Device *device;
static ID3D12CommandQueue *queue;

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = {0};
    ID3D12Resource *res = NULL;

    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size < 256 ? 256 : size;
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

static int submit_and_wait(ID3D12GraphicsCommandList *list)
{
    static UINT64 value;
    static ID3D12Fence *fence;
    HANDLE event;
    int ok;

    if (!fence && FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        return 0;
    if (FAILED(ID3D12GraphicsCommandList_Close(list)))
        return 0;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, ++value);
    event = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, value, event);
    ok = WaitForSingleObject(event, 120000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return ok;
}

static void barrier_all(ID3D12GraphicsCommandList *list, ID3D12Resource **res, int n, D3D12_RESOURCE_STATES from,
        D3D12_RESOURCE_STATES to)
{
    int i;

    for (i = 0; i < n; i++)
    {
        D3D12_RESOURCE_BARRIER b = {0};

        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res[i];
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    }
}

static unsigned int view_elem_size(const struct view *v)
{
    if (v->kind == 3)
        return 1;
    return v->stride ? v->stride : (v->flags & 1) ? 4 : sm2_format_size(v->format);
}

static uint64_t gcd64(uint64_t a, uint64_t b)
{
    while (b)
    {
        uint64_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

/* One GPU buffer per captured resource, covering every captured range of it. */
struct group
{
    uint64_t res_id, base, size;
    uint8_t *data;
    ID3D12Resource *buf;
};

static int is_float_format(uint32_t f)
{
    return f == 2 || f == 6 || f == 10 || f == 16 || f == 41 || f == 54 || f == 34 || f == 26;
}

/* Compares one UAV range; returns the number of 32-bit words that differ beyond tolerance. */
static uint64_t compare_uav(const struct view *v, const uint8_t *ref, const char *label)
{
    uint64_t words = v->byte_len / 4, i, exact = 0, bad = 0, nan_mac = 0, nan_ref = 0, zero_mac = 0, zero_ref = 0,
            changed_mac = 0, changed_ref = 0, shown = 0;
    /* typed float views: compare with a tolerance; other typed views: exactly; raw and
     * structured data: with a tolerance only where both words look like normal floats
     * (small integers would otherwise pass as tiny denormals) */
    int typed_float = is_float_format(v->format) && v->format != 34 && v->format != 54 && v->format != 10;
    int typed_other = v->format && !typed_float;
    double max_abs = 0;

    for (i = 0; i < words; i++)
    {
        uint32_t m, r, b;
        float fm, fr;

        memcpy(&m, v->after + i * 4, 4);
        memcpy(&r, ref + i * 4, 4);
        memcpy(&b, v->before + i * 4, 4);
        memcpy(&fm, &m, 4);
        memcpy(&fr, &r, 4);
        changed_mac += m != b;
        changed_ref += r != b;
        zero_mac += m == 0;
        zero_ref += r == 0;
        nan_mac += fm != fm;
        nan_ref += fr != fr;
        if (m == r)
            continue;
        exact++;
        if (!typed_other && fm == fm && fr == fr && (typed_float || (((m >> 23) & 0xff) && ((m >> 23) & 0xff) != 0xff
                && ((r >> 23) & 0xff) && ((r >> 23) & 0xff) != 0xff)))
        {
            double d = fabs((double)fm - (double)fr), tol = 1e-4 * fmax(1.0, fmax(fabs(fm), fabs(fr)));

            if (d > max_abs)
                max_abs = d;
            if (d <= tol)
                continue;
        }
        bad++;
        if (shown++ < 8)
            out("      word %llu: Mac %08x (%g)  reference %08x (%g)  before %08x\n", (unsigned long long)i, m, fm, r, fr, b);
    }
    out("    %s: %llu words; differ exactly %llu, beyond tolerance %llu (max abs diff %g)\n", label,
            (unsigned long long)words, (unsigned long long)exact, (unsigned long long)bad, max_abs);
    out("      Mac: %llu written, %llu zero, %llu NaN | reference: %llu written, %llu zero, %llu NaN\n",
            (unsigned long long)changed_mac, (unsigned long long)zero_mac, (unsigned long long)nan_mac,
            (unsigned long long)changed_ref, (unsigned long long)zero_ref, (unsigned long long)nan_ref);
    return bad;
}

/* Shader feature flags (SFI0); wave ops make results depend on the GPU's wave size. */
static uint64_t shader_flags(const uint8_t *p, size_t size)
{
    uint32_t parts, i, off;
    uint64_t f = 0;

    if (size < 32)
        return 0;
    memcpy(&parts, p + 28, 4);
    for (i = 0; i < parts && 32 + i * 4 + 4 <= size; i++)
    {
        memcpy(&off, p + 32 + i * 4, 4);
        if (off + 16 <= size && !memcmp(p + off, "SFI0", 4))
            memcpy(&f, p + off + 8, 8);
    }
    return f;
}
#define SHADER_WAVE_OPS 0x4000ull

struct summary_entry { uint64_t hash; int result, missing, wave; uint64_t bad; };
static struct summary_entry summary[4096];
static int nsummary;

static int replay(const uint8_t *exe, size_t exe_size, const char *path)
{
    struct capture c;
    struct sm2_rootsig rs;
    struct group *groups;
    ID3D12RootSignature *root_sig = NULL;
    ID3D12PipelineState *pso = NULL;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12Resource **bufs, *readback = NULL;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0;
    uint32_t table_start[ROOT_PARAMS] = {0}, table_size[ROOT_PARAMS] = {0}, p, i, j, ng = 0, inc;
    uint64_t rb_size = 0, *rb_off, bad = 0, uavs = 0, flags;
    ID3D12Resource **ups;
    int missing = 0, result;
    uint8_t *file, *shader, *map;
    size_t file_size, shader_size;
    HRESULT hr;

    out("\n== %s\n", path);
    if (!(file = read_file(path, &file_size)) || parse_capture(file, file_size, &c))
    {
        out("  FAILED: can't read the capture\n");
        return -1;
    }
    out("  shader %016llx Dispatch(%u, %u, %u), %u views\n", (unsigned long long)c.hash, c.x, c.y, c.z, c.nviews);
    for (i = 0, j = 0; i < c.nviews; i++)
        j += (c.v[i].res_id || c.v[i].kind == 3) && !c.v[i].byte_len && c.v[i].kind;
    if (j)
        out("  note: %u views (too big, or textures) weren't captured; the replay reads them as empty\n", j);
    missing = (int)j;
    if (getenv("REPLAY_SHADER"))            /* test a rewritten shader on the same capture */
    {
        shader = read_file(getenv("REPLAY_SHADER"), &shader_size);
        out("  shader replaced by %s\n", getenv("REPLAY_SHADER"));
    }
    else
        shader = find_shader(exe, exe_size, c.hash, &shader_size);
    if (!shader)
    {
        out("  FAILED: shader not found in the game executable\n");
        return -1;
    }
    flags = shader_flags(shader, shader_size);
    if (sm2_rootsig_parse(c.rs_blob, c.rs_size, &rs))
    {
        out("  FAILED: can't parse the root signature\n");
        return -1;
    }
    {
        struct sm2_binding b[64];
        int nb = sm2_shader_bindings(shader, shader_size, b, 64), k;
        uint32_t sp, ss;

        for (k = 0, j = 0; k < nb; k++)
            j += b[k].type == 1 && !sm2_rootsig_locate(&rs, 3, b[k].space, b[k].lo, &sp, &ss);
    }
    for (i = 0; i < c.nviews; i++)
        if (c.v[i].kind == 3 && !c.v[i].byte_len)
            j++;
    if (j)
    {
        out("  skipped: a constant buffer or sampler table the shader uses wasn't captured\n");
        if (nsummary < 4096)
        {
            summary[nsummary].hash = c.hash;
            summary[nsummary].result = 2;
            summary[nsummary].missing = -1;
            nsummary++;
        }
        return 2;
    }
    if (FAILED(hr = ID3D12Device_CreateRootSignature(device, 0, c.rs_blob, c.rs_size, &IID_ID3D12RootSignature,
            (void **)&root_sig)))
    {
        out("  FAILED CreateRootSignature hr=%#lx\n", (unsigned long)hr);
        return -1;
    }
    pd.pRootSignature = root_sig;
    pd.CS.pShaderBytecode = shader;
    pd.CS.BytecodeLength = shader_size;
    if (FAILED(hr = ID3D12Device_CreateComputePipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso)))
    {
        out("  FAILED CreateComputePipelineState hr=%#lx\n", (unsigned long)hr);
        return -1;
    }

    /* group the captured ranges by resource */
    groups = calloc(c.nviews + 1, sizeof(*groups));
    for (i = 0; i < c.nviews; i++)
    {
        struct view *v = &c.v[i];
        v->group = -1;
        if (!v->byte_len)
            continue;
        for (j = 0; j < ng && groups[j].res_id != v->res_id; j++)
            ;
        if (j == ng)
        {
            groups[ng].res_id = v->res_id;
            groups[ng].base = UINT64_MAX;
            ng++;
        }
        v->group = (int)j;
    }
    for (j = 0; j < ng; j++)
    {
        uint64_t lo = UINT64_MAX, hi = 0, l = 256;

        for (i = 0; i < c.nviews; i++)
        {
            struct view *v = &c.v[i];
            uint64_t es = view_elem_size(v);

            if (v->group != (int)j)
                continue;
            if (v->byte_off < lo)
                lo = v->byte_off;
            if (v->byte_off + v->byte_len > hi)
                hi = v->byte_off + v->byte_len;
            if (es)
                l = l / gcd64(l, es) * es;      /* lcm, so every view's offset stays a whole element */
        }
        groups[j].base = lo - lo % l;
        groups[j].size = (hi - groups[j].base + 255) & ~255ull;
        groups[j].data = calloc(1, groups[j].size);
        for (i = 0; i < c.nviews; i++)
            if (c.v[i].group == (int)j)
                memcpy(groups[j].data + (c.v[i].byte_off - groups[j].base), c.v[i].before, c.v[i].byte_len);
    }

    if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
            (void **)&alloc)) || FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list)))
    {
        out("  FAILED to create a command list\n");
        return -1;
    }
    bufs = calloc(ng + 1, sizeof(*bufs));
    ups = calloc(ng + 1, sizeof(*ups));
    rb_off = calloc(ng + 1, sizeof(*rb_off));
    for (j = 0; j < ng; j++)
    {
        ID3D12Resource *up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, groups[j].size, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ);
        void *m;

        bufs[j] = make_buffer(D3D12_HEAP_TYPE_DEFAULT, groups[j].size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST);
        if (!up || !bufs[j] || FAILED(ID3D12Resource_Map(up, 0, NULL, &m)))
        {
            out("  FAILED to create a %llu-byte buffer\n", (unsigned long long)groups[j].size);
            return -1;
        }
        memcpy(m, groups[j].data, groups[j].size);
        ID3D12Resource_Unmap(up, 0, NULL);
        ups[j] = up;
        ID3D12GraphicsCommandList_CopyBufferRegion(list, bufs[j], 0, up, 0, groups[j].size);
        rb_off[j] = rb_size;
        rb_size += groups[j].size;
    }
    barrier_all(list, bufs, ng, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    /* descriptor tables, one block per table parameter */
    for (p = 0; p < rs.num_params && p < ROOT_PARAMS; p++)
        if (rs.param_type[p] == SM2_RS_TABLE)
        {
            table_size[p] = 1;
            for (i = 0; i < c.nviews; i++)
                if (c.v[i].param == p && c.v[i].slot != 0xffffffffu && c.v[i].slot + 1 > table_size[p])
                    table_size[p] = c.v[i].slot + 1;
            table_start[p] = hd.NumDescriptors;
            hd.NumDescriptors += table_size[p];
        }
    if (!hd.NumDescriptors)
        hd.NumDescriptors = 1;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap)))
    {
        out("  FAILED CreateDescriptorHeap\n");
        return -1;
    }
    inc = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu0);
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu0);
    /* null descriptors of the right type everywhere first */
    for (i = 0; i < rs.num_ranges; i++)
    {
        const struct sm2_rs_range *r = &rs.ranges[i];
        uint32_t n = r->count, k;

        if (r->type == 3 || r->param >= ROOT_PARAMS || rs.param_type[r->param] != SM2_RS_TABLE)
            continue;
        for (k = 0; k < n && r->offset + k < table_size[r->param]; k++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu0.ptr + (SIZE_T)(table_start[r->param] + r->offset + k) * inc };

            if (r->type == 0)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};

                sv.Format = DXGI_FORMAT_R32_UINT;
                sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                ID3D12Device_CreateShaderResourceView(device, NULL, &sv, h);
            }
            else if (r->type == 1)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {0};

                uv.Format = DXGI_FORMAT_R32_UINT;
                uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ID3D12Device_CreateUnorderedAccessView(device, NULL, NULL, &uv, h);
            }
            else
                ID3D12Device_CreateConstantBufferView(device, NULL, h);
        }
    }
    for (i = 0; i < c.nviews; i++)
    {
        struct view *v = &c.v[i];
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        uint64_t es = view_elem_size(v), off;

        if (v->slot == 0xffffffffu || v->group < 0 || !es || v->param >= ROOT_PARAMS)
            continue;
        h.ptr = cpu0.ptr + (SIZE_T)(table_start[v->param] + v->slot) * inc;
        /* the view starts at FirstElement (the captured range may begin a few bytes earlier) */
        off = (v->kind == 1 || v->kind == 2 ? v->first * es : v->byte_off) - groups[v->group].base;
        if (v->kind == 1)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};

            sv.Format = v->format;
            sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Buffer.FirstElement = off / es;
            sv.Buffer.NumElements = v->num;
            sv.Buffer.StructureByteStride = v->stride;
            sv.Buffer.Flags = v->flags;
            /* REPLAY_RAW_SLOT=param:slot -> that 32-bit typed view becomes a raw view, as sm2fix does */
            if (getenv("REPLAY_RAW_SLOT") && (unsigned)atoi(getenv("REPLAY_RAW_SLOT")) == v->param
                    && strchr(getenv("REPLAY_RAW_SLOT"), ':') && (unsigned)atoi(strchr(getenv("REPLAY_RAW_SLOT"), ':') + 1) == v->slot
                    && es == 4)
            {
                sv.Format = DXGI_FORMAT_R32_TYPELESS;
                sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                out("  view root %u slot %u read as raw\n", v->param, v->slot);
            }
            ID3D12Device_CreateShaderResourceView(device, bufs[v->group], &sv, h);
        }
        else if (v->kind == 2)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {0};

            uv.Format = v->format;
            uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uv.Buffer.FirstElement = off / es;
            uv.Buffer.NumElements = v->num;
            uv.Buffer.StructureByteStride = v->stride;
            uv.Buffer.Flags = v->flags;
            ID3D12Device_CreateUnorderedAccessView(device, bufs[v->group], NULL, &uv, h);
        }
        else if (v->kind == 3)
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cv;

            cv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(bufs[v->group]) + off;
            cv.SizeInBytes = (UINT)((v->byte_len + 255) & ~255ull);
            ID3D12Device_CreateConstantBufferView(device, &cv, h);
        }
    }

    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root_sig);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    for (p = 0; p < rs.num_params && p < ROOT_PARAMS; p++)
    {
        if (rs.param_type[p] == SM2_RS_TABLE)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE g = { gpu0.ptr + (UINT64)table_start[p] * inc };

            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, p, g);
        }
        else if (rs.param_type[p] == SM2_RS_CONSTANTS)
            ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, p, rs.param_num[p] < 32 ? rs.param_num[p] : 32,
                    c.consts[p], 0);
        else
        {
            D3D12_GPU_VIRTUAL_ADDRESS va = 0;

            for (i = 0; i < c.nviews; i++)
                if (c.v[i].param == p && c.v[i].slot == 0xffffffffu && c.v[i].group >= 0)
                    va = ID3D12Resource_GetGPUVirtualAddress(bufs[c.v[i].group]) + (c.v[i].byte_off - groups[c.v[i].group].base);
            if (rs.param_type[p] == SM2_RS_CBV)
                ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, p, va);
            else if (rs.param_type[p] == SM2_RS_SRV)
                ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, p, va);
            else
                ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, p, va);
        }
    }
    ID3D12GraphicsCommandList_Dispatch(list, c.x, c.y, c.z);
    barrier_all(list, bufs, ng, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    readback = make_buffer(D3D12_HEAP_TYPE_READBACK, rb_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback)
    {
        out("  FAILED to create the readback buffer\n");
        return -1;
    }
    for (j = 0; j < ng; j++)
        ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, rb_off[j], bufs[j], 0, groups[j].size);
    if (!submit_and_wait(list))
    {
        out("  FAILED: the GPU didn't finish\n");
        return -1;
    }
    if (FAILED(ID3D12Resource_Map(readback, 0, NULL, (void **)&map)))
        return -1;
    for (i = 0; i < c.nviews; i++)
    {
        struct view *v = &c.v[i];
        char label[96];

        if (!v->has_after || v->group < 0)
            continue;
        snprintf(label, sizeof(label), "UAV root %u slot %d (format %u, %u elements)", v->param, (int)v->slot, v->format,
                v->num);
        bad += compare_uav(v, map + rb_off[v->group] + (v->byte_off - groups[v->group].base), label);
        uavs++;
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    result = !uavs ? -1 : !bad ? 0 : missing ? 2 : 1;
    out("  RESULT %016llx: %s (%llu UAVs compared)%s\n", (unsigned long long)c.hash,
            result < 0 ? "NOTHING TO COMPARE" : result == 0 ? "matches" : result == 2
            ? "differs, but the capture is incomplete" : "MAC OUTPUT DIFFERS FROM REFERENCE", (unsigned long long)uavs,
            flags & SHADER_WAVE_OPS ? " [uses wave ops]" : "");
    if (nsummary < 4096)
    {
        summary[nsummary].hash = c.hash;
        summary[nsummary].result = result;
        summary[nsummary].missing = missing;
        summary[nsummary].wave = !!(flags & SHADER_WAVE_OPS);
        summary[nsummary].bad = bad;
        nsummary++;
    }

    ID3D12Resource_Release(readback);
    for (j = 0; j < ng; j++)
    {
        ID3D12Resource_Release(bufs[j]);
        ID3D12Resource_Release(ups[j]);
        free(groups[j].data);
    }
    ID3D12DescriptorHeap_Release(heap);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root_sig);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    free(groups);
    free(bufs);
    free(ups);
    free(rb_off);
    free(shader);
    free(c.v);
    free(file);
    return result;
}

int main(int argc, char **argv)
{
    HRESULT (WINAPI *create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    int i, differ = 0, same = 0, failed = 0, incomplete = 0;
    size_t exe_size;
    uint8_t *exe;
    HMODULE d3d12;

    if (argc < 4)
    {
        printf("usage: %s <Spider-Man2.exe> report.txt capture.bin [...]\n", argv[0]);
        return 2;
    }
    report = fopen(argv[2], "w");
    out("Replay of captured dispatches\n");
    if (!(exe = read_file(argv[1], &exe_size)))
    {
        out("FAILED: can't read %s\n", argv[1]);
        return 1;
    }
    d3d12 = LoadLibraryA("d3d12.dll");
    create_device = d3d12 ? (void *)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    if (!create_device || (FAILED(create_device(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&device))
            && FAILED(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device))))
    {
        out("FAILED: no D3D12 device\n");
        return 1;
    }
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue)))
        return 1;
    for (i = 3; i < argc; i++)
    {
        int r = replay(exe, exe_size, argv[i]);

        differ += r == 1;
        same += r == 0;
        failed += r < 0;
        incomplete += r == 2;
    }
    out("\nShaders whose Mac output differs from the reference:\n");
    for (i = 0; i < nsummary; i++)
        if (summary[i].result == 1)
            out("  %016llx  %llu words%s\n", (unsigned long long)summary[i].hash, (unsigned long long)summary[i].bad,
                    summary[i].wave ? "  (uses wave ops: may differ legitimately)" : "");
    out("Differing but incomplete captures (some inputs too big):\n");
    for (i = 0; i < nsummary; i++)
        if (summary[i].result == 2)
            out("  %016llx  %llu words, %d views missing\n", (unsigned long long)summary[i].hash,
                    (unsigned long long)summary[i].bad, summary[i].missing);
    out("\nRESULT: %d captures match, %d differ, %d incomplete, %d couldn't be replayed\n", same, differ, incomplete, failed);
    return differ ? 1 : 0;
}
