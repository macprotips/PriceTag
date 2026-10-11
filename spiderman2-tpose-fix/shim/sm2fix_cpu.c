/* CPU-count override for the game process. The game sizes its job system from the
 * reported processor count; changing it changes how character setup overlaps with
 * asset streaming, which is where the T-pose race is suspected to live.
 *
 * Patches the import address tables of the game's main module so its calls to the
 * Win32 processor-info functions see at most `limit` logical processors. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

#include "sm2fix_cpu.h"

static unsigned int g_limit;
static KAFFINITY g_mask;

static void (WINAPI *real_GetSystemInfo)(LPSYSTEM_INFO);
static void (WINAPI *real_GetNativeSystemInfo)(LPSYSTEM_INFO);
static DWORD (WINAPI *real_GetActiveProcessorCount)(WORD);
static DWORD (WINAPI *real_GetMaximumProcessorCount)(WORD);
static BOOL (WINAPI *real_GetProcessAffinityMask)(HANDLE, PDWORD_PTR, PDWORD_PTR);
static BOOL (WINAPI *real_GetLogicalProcessorInformation)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
static BOOL (WINAPI *real_GetLogicalProcessorInformationEx)(LOGICAL_PROCESSOR_RELATIONSHIP,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);

static void clamp_system_info(LPSYSTEM_INFO info)
{
    if (info && info->dwNumberOfProcessors > g_limit)
    {
        info->dwNumberOfProcessors = g_limit;
        info->dwActiveProcessorMask &= g_mask;
    }
}

static void WINAPI hk_GetSystemInfo(LPSYSTEM_INFO info)
{
    real_GetSystemInfo(info);
    clamp_system_info(info);
}

static void WINAPI hk_GetNativeSystemInfo(LPSYSTEM_INFO info)
{
    real_GetNativeSystemInfo(info);
    clamp_system_info(info);
}

static DWORD WINAPI hk_GetActiveProcessorCount(WORD group)
{
    DWORD n = real_GetActiveProcessorCount(group);
    return n > g_limit ? g_limit : n;
}

static DWORD WINAPI hk_GetMaximumProcessorCount(WORD group)
{
    DWORD n = real_GetMaximumProcessorCount(group);
    return n > g_limit ? g_limit : n;
}

static BOOL WINAPI hk_GetProcessAffinityMask(HANDLE process, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask)
{
    BOOL ret = real_GetProcessAffinityMask(process, proc_mask, sys_mask);

    if (ret)
    {
        if (proc_mask)
            *proc_mask &= g_mask;
        if (sys_mask)
            *sys_mask &= g_mask;
    }
    return ret;
}

/* Fixed-size entries: drop processor cores outside the allowed mask, clip the rest. */
static BOOL WINAPI hk_GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD length)
{
    BOOL ret = real_GetLogicalProcessorInformation(buffer, length);
    DWORD i, n, out = 0;

    if (!ret || !buffer || !length)
        return ret;
    n = *length / sizeof(*buffer);
    for (i = 0; i < n; i++)
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION e = buffer[i];

        e.ProcessorMask &= g_mask;
        if (!e.ProcessorMask)
            continue;
        buffer[out++] = e;
    }
    *length = out * sizeof(*buffer);
    return ret;
}

/* Variable-size entries: same idea, compacting in place. */
static BOOL WINAPI hk_GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD length)
{
    BOOL ret = real_GetLogicalProcessorInformationEx(rel, buffer, length);
    BYTE *src, *dst, *end;

    if (!ret || !buffer || !length)
        return ret;
    src = dst = (BYTE *)buffer;
    end = src + *length;
    while (src < end)
    {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)src;
        DWORD size = e->Size;
        BOOL keep = TRUE;
        WORD g;

        if (!size || src + size > end)
            break;
        switch (e->Relationship)
        {
            case RelationProcessorCore:
            case RelationProcessorPackage:
                for (g = 0; g < e->Processor.GroupCount; g++)
                    e->Processor.GroupMask[g].Mask &= g_mask;
                keep = e->Processor.GroupCount && e->Processor.GroupMask[0].Mask;
                break;
            case RelationCache:
                e->Cache.GroupMask.Mask &= g_mask;
                keep = e->Cache.GroupMask.Mask != 0;
                break;
            case RelationNumaNode:
                e->NumaNode.GroupMask.Mask &= g_mask;
                break;
            case RelationGroup:
                for (g = 0; g < e->Group.ActiveGroupCount; g++)
                {
                    e->Group.GroupInfo[g].ActiveProcessorMask &= g_mask;
                    if (e->Group.GroupInfo[g].ActiveProcessorCount > g_limit)
                        e->Group.GroupInfo[g].ActiveProcessorCount = (BYTE)g_limit;
                    if (e->Group.GroupInfo[g].MaximumProcessorCount > g_limit)
                        e->Group.GroupInfo[g].MaximumProcessorCount = (BYTE)g_limit;
                }
                break;
            default:
                break;
        }
        if (keep)
        {
            if (dst != src)
                memmove(dst, src, size);
            dst += size;
        }
        src += size;
    }
    *length = (DWORD)(dst - (BYTE *)buffer);
    return ret;
}

struct hook { const char *name; void *hook; void **real; };

static struct hook g_cpu_hooks[] =
{
    {"GetSystemInfo", (void *)hk_GetSystemInfo, (void **)&real_GetSystemInfo},
    {"GetNativeSystemInfo", (void *)hk_GetNativeSystemInfo, (void **)&real_GetNativeSystemInfo},
    {"GetActiveProcessorCount", (void *)hk_GetActiveProcessorCount, (void **)&real_GetActiveProcessorCount},
    {"GetMaximumProcessorCount", (void *)hk_GetMaximumProcessorCount, (void **)&real_GetMaximumProcessorCount},
    {"GetProcessAffinityMask", (void *)hk_GetProcessAffinityMask, (void **)&real_GetProcessAffinityMask},
    {"GetLogicalProcessorInformation", (void *)hk_GetLogicalProcessorInformation,
            (void **)&real_GetLogicalProcessorInformation},
    {"GetLogicalProcessorInformationEx", (void *)hk_GetLogicalProcessorInformationEx,
            (void **)&real_GetLogicalProcessorInformationEx},
};

/* Rewrites matching by-name imports of `module`. Returns how many slots were patched. */
static unsigned int patch_imports(HMODULE module)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *desc;
    unsigned int patched = 0;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress)
        return 0;
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); desc->Name; desc++)
    {
        IMAGE_THUNK_DATA *names, *slots;

        if (!desc->OriginalFirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++)
        {
            IMAGE_IMPORT_BY_NAME *by_name;
            unsigned int i;

            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            for (i = 0; i < ARRAYSIZE(g_cpu_hooks); i++)
            {
                DWORD old;

                if (strcmp((const char *)by_name->Name, g_cpu_hooks[i].name))
                    continue;
                if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                    break;
                slots->u1.Function = (ULONG_PTR)g_cpu_hooks[i].hook;
                VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
                patched++;
                break;
            }
        }
    }
    return patched;
}

unsigned int sm2_count_process_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    unsigned int n = 0;

    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID == pid)
                n++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

unsigned int sm2_install_cpu_limit(unsigned int limit)
{
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    unsigned int i;

    if (!limit || limit > 64 || !kernel32)
        return 0;
    g_limit = limit;
    g_mask = limit >= 64 ? ~(KAFFINITY)0 : (((KAFFINITY)1 << limit) - 1);
    for (i = 0; i < ARRAYSIZE(g_cpu_hooks); i++)
    {
        *g_cpu_hooks[i].real = (void *)GetProcAddress(kernel32, g_cpu_hooks[i].name);
        if (!*g_cpu_hooks[i].real)
            g_cpu_hooks[i].hook = NULL;
    }
    /* Never install a hook whose real function is missing. */
    for (i = 0; i < ARRAYSIZE(g_cpu_hooks); i++)
    {
        if (!g_cpu_hooks[i].hook)
            g_cpu_hooks[i].name = "\x01"; /* matches nothing */
    }
    /* Affinity too, for engines that size pools from the process affinity mask. */
    SetProcessAffinityMask(GetCurrentProcess(), g_mask);
    return patch_imports(GetModuleHandleA(NULL));
}
