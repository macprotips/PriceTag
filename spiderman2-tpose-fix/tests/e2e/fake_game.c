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
static const char skin_shader[] = "stand-in-for-CS_ModelSkinning";

/* Stand-in for the game's skin-job launcher: same first 15 bytes (three register saves),
 * then mgr[0x34] = mgr[0x30] + 1 so the test can see the original code still ran. */
__asm__(".text\n.globl skin_launcher_test\nskin_launcher_test:\n"
        "  movq %rbx, 8(%rsp)\n  movq %rbp, 16(%rsp)\n  movq %rsi, 24(%rsp)\n"
        "  movl 0x30(%rcx), %eax\n  addl $1, %eax\n  movl %eax, 0x34(%rcx)\n  ret\n");
void skin_launcher_test(void *mgr);
static uint32_t g_fake_jobsys[32];

/* Stand-ins for the SkinningSB stub (same instruction encodings as the game's) and the
 * skin dispatch function (same 13-byte prologue, then movsxd rsi, [queued count]). */
int32_t fake_render_budgets[28] = {100, 200, 300, 16};
uint32_t fake_skinning_sb[8];
int32_t fake_skin_count = 5, fake_skin_seen;
int32_t fake_queue_sel, fake_queue_alt;
/* 5 queue items of 24 bytes: +8 = bone palette base (in 64-byte bones) */
static int32_t fake_queue_items[5 * 6] = { 0, 0, 0, 0, 0, 0,  0, 0, 4, 0, 0, 0,  0, 0, -1, 0, 0, 0,
                                           0, 0, 8, 0, 0, 0,  0, 0, 12, 0, 0, 0 };
void *fake_queue = fake_queue_items;
void (*fake_dispatch_cb)(void);   /* called from inside the stand-in dispatch function */
static ID3D12GraphicsCommandList *g_cb_list;
static D3D12_GPU_DESCRIPTOR_HANDLE g_cb_table;
static void bind_table_cb(void)
{
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(g_cb_list, 1, g_cb_table);
}
const char fake_sb_name[] = "SkinningSB";
uint64_t fake_sb_create(uint32_t *object, const char *name, uint32_t count, uint32_t stride)
{
    object[4] = count;
    object[5] = stride;
    return name == fake_sb_name;
}
__asm__(".text\n.globl skin_sb_stub_test\nskin_sb_stub_test:\n"
        "  movl fake_render_budgets+12(%rip), %r8d\n  leaq fake_sb_name(%rip), %rdx\n"
        "  movl $0x20, %r9d\n  leaq fake_skinning_sb(%rip), %rcx\n"
        "  .byte 0xe9\n  .long fake_sb_create - . - 4\n"
        ".globl skin_dispatch_test\nskin_dispatch_test:\n"
        "  .byte 0x4c, 0x8b, 0xdc, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00\n"
        "  movslq fake_skin_count(%rip), %rsi\n"                   /* +0x0d */
        "  xorl %eax, %eax\n"                                      /* +0x14 */
        "  cmpl %eax, fake_queue_sel(%rip)\n"                      /* +0x16 */
        "  .byte 0x0f, 0x1f, 0x80, 0, 0, 0, 0\n"                   /* +0x1c nop7 */
        "  cmovnel fake_queue_alt(%rip), %eax\n"                   /* +0x23 */
        "  .byte 0x66, 0x0f, 0x1f, 0x44, 0, 0\n"                   /* +0x2a nop6 */
        "  movq fake_queue(%rip), %rax\n"                          /* +0x30 */
        "  movl %esi, fake_skin_seen(%rip)\n"
        "  movq fake_dispatch_cb(%rip), %rax\n  testq %rax, %rax\n  jz 1f\n  call *%rax\n1:\n"
        "  addq $0xc8, %rsp\n  popq %r15\n  popq %rsi\n  ret\n");
uint64_t skin_sb_stub_test(void);
void skin_dispatch_test(void);

/* Fake RTTI + vtables for the three animation config classes force_anim looks for. */
struct fake_td { void *vt; void *spare; char name[48]; };
struct fake_col { uint32_t sig, off, cd, td_rva, cd_rva, self_rva; };
static struct fake_td fake_tds[3] = {
    { NULL, NULL, ".?AVAnimUpdateLODPrius@@" },
    { NULL, NULL, ".?AVAnimControllerComponentPrius@@" },
    { NULL, NULL, ".?AVSimpleAnimComponentPrius@@" },
};
static struct fake_col fake_cols[3];
static void *fake_vtables[3][4];    /* [0] = COL pointer, the "vtable" starts at [1] */

/* Fake skinning manager for trap_writer: +0x58 palette copy list, +0x60 count. */
static uint8_t fake_mgr[0x100];
static uint8_t *fake_src_page;
static void __attribute__((noinline)) write_bones(volatile float *dst)
{
    dst[0] = 0.5f;          /* the "animation code" writing a bone matrix */
}

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
    {
        char rva[32];
        snprintf(rva, sizeof(rva), "%llx", (unsigned long long)((uint8_t *)skin_launcher_test - (uint8_t *)GetModuleHandleA(NULL)));
        SetEnvironmentVariableA("SM2FIX_PROBE_SKIN_RVA", rva);
        snprintf(rva, sizeof(rva), "%llx", (unsigned long long)((uint8_t *)g_fake_jobsys - (uint8_t *)GetModuleHandleA(NULL)));
        SetEnvironmentVariableA("SM2FIX_PROBE_JOBSYS_RVA", rva);
        SetEnvironmentVariableA("SM2FIX_PROBE_SKIN", !strcmp(mode, "off") ? "0" : "1");
        snprintf(rva, sizeof(rva), "%llx", (unsigned long long)((uint8_t *)skin_sb_stub_test - (uint8_t *)GetModuleHandleA(NULL)));
        SetEnvironmentVariableA("SM2FIX_PROBE_SB_RVA", rva);
        snprintf(rva, sizeof(rva), "%llx", (unsigned long long)((uint8_t *)skin_dispatch_test - (uint8_t *)GetModuleHandleA(NULL)));
        SetEnvironmentVariableA("SM2FIX_PROBE_DISPATCH_RVA", rva);
        /* hash: budget raised; all/max: probe only (budget 0); off: nothing hooked */
        SetEnvironmentVariableA("SM2FIX_SKIN_BUDGET", !strcmp(mode, "hash") ? "65536" : "0");
        g_fake_jobsys[9] = 7; /* the gate flag at +0x24 */
    }
    snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)sm2_shader_hash(skin_shader, sizeof(skin_shader)));
    SetEnvironmentVariableA("SM2FIX_SKINNING_HASH", hash);
    SetEnvironmentVariableA("SM2FIX_REPLACE_SKINNING", !strcmp(mode, "hash") ? "1" : "0");
    SetEnvironmentVariableA("SM2FIX_SNAPSHOT_MS", "0");
    {
        uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
        char rvas[64];
        int k;

        for (k = 0; k < 3; k++)
        {
            fake_cols[k].sig = 1;
            fake_cols[k].td_rva = (uint32_t)((uint8_t *)&fake_tds[k] - base);
            fake_vtables[k][0] = &fake_cols[k];
        }
        snprintf(rvas, sizeof(rvas), "%x,%x,%x", (unsigned)((uint8_t *)&fake_vtables[0][1] - base),
                (unsigned)((uint8_t *)&fake_vtables[1][1] - base), (unsigned)((uint8_t *)&fake_vtables[2][1] - base));
        SetEnvironmentVariableA("SM2FIX_ANIM_VTBL_RVAS", rvas);
        SetEnvironmentVariableA("SM2FIX_FORCE_ANIM", !strcmp(mode, "off") ? "0" : "1");
        SetEnvironmentVariableA("SM2FIX_FORCE_ANIM_DELAY_MS", "0");
        snprintf(rvas, sizeof(rvas), "%x", (unsigned)(fake_mgr - base));
        SetEnvironmentVariableA("SM2FIX_MANAGER_RVA", rvas);
        SetEnvironmentVariableA("SM2FIX_TRAP_WRITER", !strcmp(mode, "off") ? "0" : "1");
    }
    SetEnvironmentVariableA("SM2FIX_TRACE_QUERIES", "1");
    /* all/max runs: mock behaves like D3DMetal (60 Hz, zero timestamps) and the shim fakes */
    SetEnvironmentVariableA("SM2FIX_FAKE_TIMESTAMPS", (!strcmp(mode, "all") || !strcmp(mode, "max")) ? "1" : "0");
    SetEnvironmentVariableA("SM2FIX_PSO_DELAY_MS", !strcmp(mode, "max") ? "50" : "0");
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

    /* force_anim: config objects in private memory get "always animate" values. */
    {
        uint8_t *objs = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        uint8_t *lod = objs + 0x100, *ctrl = objs + 0x200, *simple = objs + 0x300;
        int tries;

        *(void **)lod = &fake_vtables[0][1];
        *(float *)(lod + 0xc) = 20.0f;
        *(float *)(lod + 0x10) = 60.0f;
        *(void **)ctrl = &fake_vtables[1][1];
        *(float *)(ctrl + 0x60) = 35.0f;
        *(float *)(ctrl + 0x64) = 1.0f;
        ctrl[0x68] = 0;
        ctrl[0x6a] = 1;
        *(void **)simple = &fake_vtables[2][1];
        *(float *)(simple + 0x20) = 50.0f;
        *(float *)(simple + 0x24) = 10.0f;
        for (tries = 0; tries < 100 && !(ctrl[0x68] && *(float *)(lod + 0xc) > 1e5f && *(float *)(simple + 0x24) == 0.0f); tries++)
            Sleep(50);
        printf("force_anim: RangeBase=%g NeverCull=%u AutoFreeze=%u MaxUpdate=%g Culled=%g\n", *(float *)(lod + 0xc),
                ctrl[0x68], ctrl[0x6a], *(float *)(ctrl + 0x60), *(float *)(simple + 0x24));
        if (strcmp(mode, "off"))
            CHECK(*(float *)(lod + 0xc) >= 1e6f && *(float *)(lod + 0x10) >= 1e6f && ctrl[0x68] == 1 && ctrl[0x6a] == 0
                    && *(float *)(ctrl + 0x60) == 0.0f && *(float *)(simple + 0x20) == 0.0f && *(float *)(simple + 0x24) == 0.0f);
        else
            CHECK(*(float *)(lod + 0xc) == 20.0f && ctrl[0x68] == 0);
    }

    /* Skin probe: the hooked function must still do its job, and the hook must count it. */
    {
        uint32_t mgr[64] = {0};
        int k;

        mgr[12] = 41;   /* +0x30 requests */
        for (k = 0; k < 3; k++)
            skin_launcher_test(mgr);
        printf("skin probe: original ran -> mgr[0x34]=%u (want 42), first byte now %#x\n", mgr[13],
                *(uint8_t *)skin_launcher_test);
        CHECK(mgr[13] == 42);
        CHECK(!strcmp(mode, "off") ? *(uint8_t *)skin_launcher_test == 0x48 && ((uint8_t *)skin_launcher_test)[1] == 0x89
                                   : ((uint8_t *)skin_launcher_test)[1] == 0xb8);
    }

    /* SkinningSB: the stub must still create the buffer, with the raised budget in hash mode. */
    {
        uint64_t ok = skin_sb_stub_test();
        int k;

        printf("SkinningSB: ok=%llu capacity=%u stride=%u budget=%d\n", (unsigned long long)ok,
                fake_skinning_sb[4], fake_skinning_sb[5], fake_render_budgets[3]);
        CHECK(ok == 1 && fake_skinning_sb[5] == 32);
        CHECK(fake_skinning_sb[4] == (!strcmp(mode, "hash") ? 65536u : 16u));
        CHECK(fake_render_budgets[3] == (int32_t)fake_skinning_sb[4]);
        CHECK(fake_render_budgets[0] == 100 && fake_render_budgets[2] == 300);
        for (k = 0; k < 3; k++)
            skin_dispatch_test();
        CHECK(fake_skin_seen == 5);
        CHECK(!strcmp(mode, "off") ? ((uint8_t *)skin_dispatch_test)[0] == 0x4c : ((uint8_t *)skin_dispatch_test)[1] == 0xb8);
    }

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
    {
        DWORD t0 = GetTickCount(), dt;

        CHECK(SUCCEEDED(ID3D12Device4_CreateComputePipelineState(device, &cdesc, &IID_ID3D12PipelineState, (void **)&pso_target)));
        dt = GetTickCount() - t0;
        /* pso_delay_ms=50 in max mode: creation must take at least ~50 ms, else be quick. */
        printf("pso creation took %lu ms\n", dt);
        CHECK(!strcmp(mode, "max") ? dt >= 40 : dt < 40);
    }
    cdesc.CS.pShaderBytecode = other_shader;
    cdesc.CS.BytecodeLength = sizeof(other_shader);
    CHECK(SUCCEEDED(ID3D12Device4_CreateComputePipelineState(device, &cdesc, &IID_ID3D12PipelineState, (void **)&pso_other)));

    /* replace_skinning (hash mode): the driver must get sm2fix-skinning.dxil instead of
     * the skinning shader; otherwise the original. run.sh writes that file. */
    {
        unsigned long long (*last_cs_hash)(void) = (void *)GetProcAddress(mock, "mock_get_last_cs_hash");
        ID3D12PipelineState *pso_skin = NULL;
        static const char replacement[] = "DXBC-pretend-replacement-skinning-shader-0123456789";

        cdesc.CS.pShaderBytecode = skin_shader;
        cdesc.CS.BytecodeLength = sizeof(skin_shader);
        CHECK(SUCCEEDED(ID3D12Device4_CreateComputePipelineState(device, &cdesc, &IID_ID3D12PipelineState, (void **)&pso_skin)));
        printf("skinning PSO: driver got shader %016llx\n", last_cs_hash());
        CHECK(last_cs_hash() == (!strcmp(mode, "hash") ? sm2_shader_hash(replacement, sizeof(replacement) - 1)
                                                        : sm2_shader_hash(skin_shader, sizeof(skin_shader))));
    }

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

    /* Descriptor tracking (probe_skin): an SRV created in a CPU heap, copied into a
     * shader-visible heap, then bound as a table inside the skin dispatch function. */
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
        D3D12_CPU_DESCRIPTOR_HANDLE cpu, vis_cpu;
        ID3D12DescriptorHeap *cpu_heap, *vis_heap;
        void *(*readback)(void) = (void *)GetProcAddress(mock, "mock_get_readback_resource");
        float *bones;
        int b, k;

        /* bone palettes in the mock's CPU-visible buffer: 0 and 8 identity, 4 and 12 animated */
        CHECK(SUCCEEDED(ID3D12Resource_Map((ID3D12Resource *)readback(), 0, NULL, (void **)&bones)));
        for (b = 0; b < 16; b++)
            for (k = 0; k < 16; k++)
                bones[(7 + b) * 16 + k] = (k % 5 == 0) ? ((b / 4) % 2 ? 0.5f : 1.0f) : 0.0f;

        CHECK(SUCCEEDED(ID3D12Device4_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap)));
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK(SUCCEEDED(ID3D12Device4_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&vis_heap)));
        cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(cpu_heap, &cpu);
        vis_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(vis_heap, &vis_cpu);
        vis_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(vis_heap, &g_cb_table);
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.FirstElement = 7;
        sv.Buffer.NumElements = 10;
        sv.Buffer.StructureByteStride = 64;
        cpu.ptr += 3 * 32;
        ID3D12Device4_CreateShaderResourceView(device, readback(), &sv, cpu);
        vis_cpu.ptr += 2 * 32;
        ID3D12Device4_CopyDescriptorsSimple(device, 1, vis_cpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_cb_table.ptr += 2 * 32;
        {
            void *(*rb)(void) = (void *)GetProcAddress(mock, "mock_get_rb_resource");

            /* a GPU->CPU copy the readback monitor should pick up */
            ID3D12GraphicsCommandList_CopyBufferRegion(list, rb(), 0, readback(), 0, 64);
        }
        g_cb_list = list;
        fake_dispatch_cb = bind_table_cb;
        skin_dispatch_test();
        /* trap_writer: copy list with an identity (palette 0) and an animated (palette 4) entry */
        {
            uint32_t *list_entries;
            volatile float *dst;

            fake_src_page = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            list_entries = (uint32_t *)(fake_src_page + 4096);
            list_entries[0] = 0;  list_entries[1] = 4 * 64;  *(void **)&list_entries[2] = fake_src_page + 512;
            list_entries[4] = 4 * 64;  list_entries[5] = 4 * 64;  *(void **)&list_entries[6] = fake_src_page + 1024;
            *(float *)(fake_src_page + 512) = 1.0f;
            *(void **)(fake_mgr + 0x58) = list_entries;
            *(int32_t *)(fake_mgr + 0x60) = 2;
            skin_dispatch_test();           /* snapshot: arms the trap on palette 4's source */
            dst = (volatile float *)(fake_src_page + 1024);
            printf("trap: read elsewhere on the page -> %g\n", *(volatile float *)(fake_src_page + 8));
            write_bones(dst + 4);
            printf("trap: wrote %g\n", dst[4]);
            CHECK(dst[4] == 0.5f);
        }
        fake_dispatch_cb = NULL;
    }

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
        /* Query tracing: timestamps resolved into a readback buffer get decoded in the log. */
        if (!strcmp(mode, "all") || !strcmp(mode, "max"))
        {
            void (*d3dmetal_like)(int) = (void *)GetProcAddress(mock, "mock_set_d3dmetal_like");
            void *(*readback)(void) = (void *)GetProcAddress(mock, "mock_get_readback_resource");
            D3D12_QUERY_HEAP_DESC qh = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 16, 0};
            ID3D12QueryHeap *heap = NULL;
            UINT64 freq = 0, *vals, gpu = 0, cpu = 0;
            LARGE_INTEGER qf;
            void *mapped = NULL;

            d3dmetal_like(1);
            QueryPerformanceFrequency(&qf);
            CHECK(SUCCEEDED(ID3D12Device4_CreateQueryHeap(device, &qh, &IID_ID3D12QueryHeap, (void **)&heap)));
            ID3D12CommandQueue_GetTimestampFrequency(queue, &freq);
            ID3D12CommandQueue_GetClockCalibration(queue, &gpu, &cpu);
            ID3D12GraphicsCommandList_EndQuery(list, heap, D3D12_QUERY_TYPE_TIMESTAMP, 3);
            Sleep(20);
            ID3D12GraphicsCommandList_EndQuery(list, heap, D3D12_QUERY_TYPE_TIMESTAMP, 4);
            ID3D12GraphicsCommandList_ResolveQueryData(list, heap, D3D12_QUERY_TYPE_TIMESTAMP, 3, 2,
                    (ID3D12Resource *)readback(), 0);
            ((ID3D12Resource *)readback())->lpVtbl->Map((ID3D12Resource *)readback(), 0, NULL, &mapped);
            vals = mapped;
            printf("fake timestamps: freq=%llu (QPC %lld) t3=%llu t4=%llu delta=%.1f ms calib gpu=%llu cpu=%llu\n",
                    (unsigned long long)freq, (long long)qf.QuadPart, (unsigned long long)vals[0],
                    (unsigned long long)vals[1], (double)(vals[1] - vals[0]) * 1000.0 / (double)freq,
                    (unsigned long long)gpu, (unsigned long long)cpu);
            CHECK(freq == (UINT64)qf.QuadPart);
            CHECK(vals[0] != 0 && vals[1] > vals[0]);
            CHECK((double)(vals[1] - vals[0]) * 1000.0 / (double)freq >= 15.0);
            CHECK(gpu != 0 && cpu != 0);
            d3dmetal_like(0);
        }
        else
        {
            void *(*readback)(void) = (void *)GetProcAddress(mock, "mock_get_readback_resource");
            UINT64 freq = 0;

            ID3D12CommandQueue_GetTimestampFrequency(queue, &freq);
            ID3D12GraphicsCommandList_EndQuery(list, NULL, D3D12_QUERY_TYPE_TIMESTAMP, 0);
            ID3D12GraphicsCommandList_ResolveQueryData(list, NULL, D3D12_QUERY_TYPE_TIMESTAMP, 0, 3,
                    (ID3D12Resource *)readback(), 0);
        }
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
