/* Plays the part of Spider-Man2.exe: loads the shim as d3d12.dll, builds pipelines and
 * dispatches, then asks the mock driver how many UAV barriers the shim injected. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

#include "../../shim/sm2fix_core.h"
#include "../../shim/sm2fix_dxbc.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static const char target_shader[] = "pretend-this-is-the-DXIL-of-the-racy-shader";
static const char other_shader[] = "some-unrelated-compute-shader";

struct sub_ptr { UINT type; UINT pad; void *p; };
struct sub_bytecode { UINT type; UINT pad; const void *code; UINT64 size; };

int main(int argc, char **argv)
{
    HRESULT (WINAPI *create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    void (*get_counts)(LONG *, LONG *, LONG *);
    void (*mock_get_queue)(LONG *, LONG *, LONG *);
    const char *mode = argc > 1 ? argv[1] : "hash";
    ID3D12PipelineState *pso_target, *pso_other, *pso_stream;
    ID3D12GraphicsCommandList *list, *bundle;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cdesc;
    D3D12_PIPELINE_STATE_STREAM_DESC sdesc;
    struct { struct sub_ptr root; struct sub_bytecode cs; } stream;
    LONG uav, other, dispatches;
    ID3D12Device4 *device;
    HMODULE shim, mock;
    char hash[32];

    snprintf(hash, sizeof(hash), "%016llx",
            (unsigned long long)sm2_shader_hash(target_shader, sizeof(target_shader)));
    SetEnvironmentVariableA("SM2FIX_HASHES", hash);
    SetEnvironmentVariableA("SM2FIX_MODE", mode);
    SetEnvironmentVariableA("SM2FIX_REAL_D3D12", "mock_d3d12.dll");
    SetEnvironmentVariableA("SM2FIX_LOG_SHADERS", "1");
    SetEnvironmentVariableA("SM2FIX_SPOOF", "sm65, no_int64,no_fp16");
    SetEnvironmentVariableA("SM2FIX_DUMP_COMPUTE", "1");
    SetEnvironmentVariableA("SM2FIX_PROFILE", "1");
    SetEnvironmentVariableA("SM2FIX_CPU_COUNT", !strcmp(mode, "off") ? "0" : "2");
    SetEnvironmentVariableA("SM2FIX_MINMAX", !strcmp(mode, "off") ? "keep" : "linear");

    shim = LoadLibraryA(".\\d3d12.dll");
    mock = LoadLibraryA("mock_d3d12.dll");
    if (!shim || !mock)
    {
        printf("FAIL: could not load shim (%p) or mock (%p)\n", (void *)shim, (void *)mock);
        return 1;
    }
    create_device = (void *)GetProcAddress(shim, "D3D12CreateDevice");

    /* CPU-count override: everything the game can ask must report <= 2 processors. */
    {
        SYSTEM_INFO si;
        DWORD len = 0, cores = 0, procs_in_cores = 0;
        DWORD_PTR pmask = 0, smask = 0;
        BYTE *buf, *p;
        int limited = strcmp(mode, "off") != 0;

        GetSystemInfo(&si);
        GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
        buf = HeapAlloc(GetProcessHeap(), 0, len);
        CHECK(GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len));
        for (p = buf; p < buf + len; p += ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p)->Size)
        {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
            if (e->Relationship == RelationProcessorCore)
            {
                cores++;
                procs_in_cores += (DWORD)__builtin_popcountll(e->Processor.GroupMask[0].Mask);
            }
        }
        GetProcessAffinityMask(GetCurrentProcess(), &pmask, &smask);
        printf("cpu: GetSystemInfo=%lu active=%lu ex_cores=%lu ex_procs=%lu affinity=%#llx\n",
                si.dwNumberOfProcessors, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), cores, procs_in_cores,
                (unsigned long long)pmask);
        if (limited)
        {
            CHECK(si.dwNumberOfProcessors <= 2);
            CHECK(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) <= 2);
            CHECK(procs_in_cores <= 2 && cores >= 1);
            CHECK(pmask && !(pmask & ~(DWORD_PTR)3));
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    get_counts = (void *)GetProcAddress(mock, "mock_get_counts");

    CHECK(SUCCEEDED(create_device(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device4, (void **)&device)));

    memset(&cdesc, 0, sizeof(cdesc));
    cdesc.CS.pShaderBytecode = target_shader;
    cdesc.CS.BytecodeLength = sizeof(target_shader);
    CHECK(SUCCEEDED(ID3D12Device4_CreateComputePipelineState(device, &cdesc, &IID_ID3D12PipelineState, (void **)&pso_target)));
    cdesc.CS.pShaderBytecode = other_shader;
    cdesc.CS.BytecodeLength = sizeof(other_shader);
    CHECK(SUCCEEDED(ID3D12Device4_CreateComputePipelineState(device, &cdesc, &IID_ID3D12PipelineState, (void **)&pso_other)));

    memset(&stream, 0, sizeof(stream));
    stream.root.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
    stream.cs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
    stream.cs.code = target_shader;
    stream.cs.size = sizeof(target_shader);
    sdesc.SizeInBytes = sizeof(stream);
    sdesc.pPipelineStateSubobjectStream = &stream;
    CHECK(SUCCEEDED(ID3D12Device4_CreatePipelineState(device, &sdesc, &IID_ID3D12PipelineState, (void **)&pso_stream)));

    CHECK(SUCCEEDED(ID3D12Device4_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, NULL, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list)));
    CHECK(SUCCEEDED(ID3D12Device4_CreateCommandList1(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            D3D12_COMMAND_LIST_FLAG_NONE, &IID_ID3D12GraphicsCommandList, (void **)&bundle)));

    ID3D12GraphicsCommandList_SetPipelineState(list, pso_target);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);           /* target          */
    ID3D12GraphicsCommandList_SetPipelineState(list, pso_other);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);           /* not target      */
    ID3D12GraphicsCommandList_Reset(list, NULL, pso_target);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);           /* target via Reset */
    ID3D12GraphicsCommandList_ClearState(list, NULL);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);           /* nothing bound   */
    ID3D12GraphicsCommandList_SetPipelineState(list, pso_stream);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);           /* target via stream PSO */
    ID3D12GraphicsCommandList_ExecuteIndirect(list, NULL, 1, NULL, 0, NULL, 0); /* indirect */
    ID3D12GraphicsCommandList_SetPipelineState(bundle, pso_target);
    ID3D12GraphicsCommandList_Dispatch(bundle, 1, 1, 1);         /* bundle: never   */

    get_counts(&uav, &other, &dispatches);
    printf("mode=%s dispatches=%ld injected_uav_barriers=%ld other_barriers=%ld\n", mode, dispatches, uav, other);
    CHECK(dispatches == 6);
    CHECK(other == 0);
    if (!strcmp(mode, "hash"))
        CHECK(uav == 3);
    else if (!strcmp(mode, "all") || !strcmp(mode, "max"))
        CHECK(uav == 6); /* 5 dispatches + 1 ExecuteIndirect; never the bundle */
    else
        CHECK(uav == 0);

    /* Diagnostics: failing graphics PSOs get logged + dumped, features logged, queues hooked. */
    {
        static const char vs[] = "vertex-shader-that-fails", ps[] = "pixel-shader-that-fails";
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gdesc;
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts;
        D3D12_COMMAND_QUEUE_DESC qdesc;
        ID3D12CommandQueue *queue = NULL;
        ID3D12PipelineState *gpso = NULL;
        LONG signals, executes, indirect;
        char dump[128];
        int i;

        memset(&gdesc, 0, sizeof(gdesc));
        gdesc.VS.pShaderBytecode = vs;
        gdesc.VS.BytecodeLength = sizeof(vs);
        gdesc.PS.pShaderBytecode = ps;
        gdesc.PS.BytecodeLength = sizeof(ps);
        CHECK(FAILED(ID3D12Device4_CreateGraphicsPipelineState(device, &gdesc, &IID_ID3D12PipelineState, (void **)&gpso)));
        snprintf(dump, sizeof(dump), "sm2fix-dumps\\%016llx_vs.dxil",
                (unsigned long long)sm2_shader_hash(vs, sizeof(vs)));
        CHECK(GetFileAttributesA(dump) != INVALID_FILE_ATTRIBUTES);

        CHECK(SUCCEEDED(ID3D12Device4_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))));

        /* Spoofing: the mock answers 0xab.. everywhere; the shim must rewrite these. */
        {
            D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1;
            D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4;
            D3D12_FEATURE_DATA_ARCHITECTURE arch = {0};

            ID3D12Device4_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
            ID3D12Device4_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
            ID3D12Device4_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
            ID3D12Device4_CheckFeatureSupport(device, D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch));
            CHECK(sm.HighestShaderModel == 0x65);
            CHECK(o1.Int64ShaderOps == 0 && o1.WaveOps == (BOOL)0xabababab);
            CHECK(o4.Native16BitShaderOpsSupported == 0);
            CHECK(arch.UMA == (BOOL)0xabababab); /* "uma" not requested: untouched */
        }

        /* dump_compute=1: every compute shader is saved. */
        snprintf(dump, sizeof(dump), "sm2fix-dumps\\%016llx_cs.dxil",
                (unsigned long long)sm2_shader_hash(other_shader, sizeof(other_shader)));
        CHECK(GetFileAttributesA(dump) != INVALID_FILE_ATTRIBUTES);

        memset(&qdesc, 0, sizeof(qdesc));
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        CHECK(SUCCEEDED(ID3D12Device4_CreateCommandQueue(device, &qdesc, &IID_ID3D12CommandQueue, (void **)&queue)));
        for (i = 0; i < 3; i++)
            ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList *const *)&list);
        mock_get_queue = (void *)GetProcAddress(mock, "mock_get_queue_counts");
        mock_get_queue(&signals, &executes, &indirect);
        printf("queue: executes=%ld shim_signals=%ld indirect=%ld\n", executes, signals, indirect);
        CHECK(executes == 3);

        /* Min/max samplers: heap sampler and root-signature static sampler get rewritten
         * (minmax=linear), and the edited root signature still has a valid checksum. */
        {
            void (*sampler_info)(UINT *, UINT *, int *) = (void *)GetProcAddress(mock, "mock_get_sampler_info");
            static uint8_t blob[36 + 8 + 24 + 52];
            uint32_t hdr[9] = {0}, rts0[6 + 13] = {0}, part = sizeof(rts0), sum[4];
            D3D12_SAMPLER_DESC sdesc;
            ID3D12RootSignature *rs = NULL;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = {0};
            UINT heap_filter, rs_filter;
            int checksum_ok;
            int keep = !strcmp(mode, "off");

            memcpy(hdr, "DXBC", 4);
            hdr[5] = 1; hdr[6] = sizeof(blob); hdr[7] = 1; hdr[8] = sizeof(hdr);
            rts0[0] = 2; rts0[3] = 1; rts0[4] = 24; rts0[6] = 0x195;
            memcpy(blob, hdr, sizeof(hdr));
            memcpy(blob + 36, "RTS0", 4);
            memcpy(blob + 40, &part, 4);
            memcpy(blob + 44, rts0, sizeof(rts0));
            sm2_dxbc_checksum(blob, sizeof(blob), sum);
            memcpy(blob + 4, sum, 16);
            CHECK(SUCCEEDED(ID3D12Device4_CreateRootSignature(device, 0, blob, sizeof(blob),
                    &IID_ID3D12RootSignature, (void **)&rs)));

            memset(&sdesc, 0, sizeof(sdesc));
            sdesc.Filter = D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR;
            ID3D12Device4_CreateSampler(device, &sdesc, handle);

            sampler_info(&heap_filter, &rs_filter, &checksum_ok);
            printf("samplers: heap filter %#x, root sig filter %#x, checksum ok %d\n", heap_filter, rs_filter, checksum_ok);
            CHECK(heap_filter == (keep ? 0x115u : 0x15u));
            CHECK(rs_filter == (keep ? 0x195u : 0x15u));
            CHECK(checksum_ok);
            CHECK(rts0[6] == 0x195); /* the game's own blob is never modified */
            CHECK(!memcmp(blob + 44 + 24, &rts0[6], 4));
        }
        CHECK(indirect == 1);
        /* MAX mode signals its own fence after every submission and waits for it. */
        CHECK(signals == (!strcmp(mode, "max") ? 3 : 0));
    }

    ID3D12GraphicsCommandList_Release(list);
    ID3D12GraphicsCommandList_Release(bundle);
    printf(failures ? "E2E FAILED\n" : "E2E OK\n");
    return failures ? 1 : 0;
}
