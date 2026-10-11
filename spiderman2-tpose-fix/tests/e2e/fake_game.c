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

    shim = LoadLibraryA(".\\d3d12.dll");
    mock = LoadLibraryA("mock_d3d12.dll");
    if (!shim || !mock)
    {
        printf("FAIL: could not load shim (%p) or mock (%p)\n", (void *)shim, (void *)mock);
        return 1;
    }
    create_device = (void *)GetProcAddress(shim, "D3D12CreateDevice");
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
    ID3D12GraphicsCommandList_SetPipelineState(bundle, pso_target);
    ID3D12GraphicsCommandList_Dispatch(bundle, 1, 1, 1);         /* bundle: never   */

    get_counts(&uav, &other, &dispatches);
    printf("mode=%s dispatches=%ld injected_uav_barriers=%ld other_barriers=%ld\n", mode, dispatches, uav, other);
    CHECK(dispatches == 6);
    CHECK(other == 0);
    if (!strcmp(mode, "hash"))
        CHECK(uav == 3);
    else if (!strcmp(mode, "all"))
        CHECK(uav == 5);
    else
        CHECK(uav == 0);

    ID3D12GraphicsCommandList_Release(list);
    ID3D12GraphicsCommandList_Release(bundle);
    printf(failures ? "E2E FAILED\n" : "E2E OK\n");
    return failures ? 1 : 0;
}
