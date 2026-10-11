/* Runs every instruction form the game uses (harvested by harvest.py, assembled into
 * tests.h) on many deterministic inputs and hashes the complete CPU state afterwards.
 *
 *   rosetta_fuzz.exe generate golden.bin          (on a real x86 CPU: writes the reference)
 *   rosetta_fuzz.exe check golden.bin report.txt  (under Rosetta: compares, writes a report)
 *
 * Each test is one instruction followed by ret. A small stub loads all general purpose
 * registers, YMM0-15, RFLAGS and MXCSR from a state block (r15 points into the block's
 * scratch memory, which every memory operand was rewritten to use), calls the test and
 * stores everything back. Data (registers, vectors, memory), arithmetic flags and MXCSR
 * status bits are compared separately, so an undefined-flag difference isn't mistaken for
 * a wrong result. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests.h"
#include "forms.h"

#define NINPUTS 48
#define NENVS 2
#define MEM_SIZE 512
#define MEM_R15 128                 /* r15 = &mem[128]; operands use [r15 + 0x40] */

struct state
{
    uint64_t gpr[16];               /* rax rcx rdx rbx rsp rbp rsi rdi r8 .. r15 */
    uint8_t ymm[16][32];
    uint64_t flags;
    uint32_t mxcsr, pad;
    uint8_t pad2[48];
    uint8_t mem[MEM_SIZE];
};

#define OFF_GPR 0
#define OFF_YMM 128
#define OFF_FLAGS (128 + 512)
#define OFF_MXCSR (OFF_FLAGS + 8)

void run_test(struct state *s, const void *code);
void *volatile g_code;
struct state *volatile g_state;

/* Windows x64: rcx = state, rdx = code. Saves the callee-saved registers (including
 * xmm6-15), loads the whole state, calls the test, stores the state back. */
__asm__(
    ".text\n.globl run_test\nrun_test:\n"
    "  pushq %rbx\n  pushq %rbp\n  pushq %rdi\n  pushq %rsi\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $168, %rsp\n"
    "  movdqu %xmm6, 0(%rsp)\n  movdqu %xmm7, 16(%rsp)\n  movdqu %xmm8, 32(%rsp)\n  movdqu %xmm9, 48(%rsp)\n"
    "  movdqu %xmm10, 64(%rsp)\n  movdqu %xmm11, 80(%rsp)\n  movdqu %xmm12, 96(%rsp)\n  movdqu %xmm13, 112(%rsp)\n"
    "  movdqu %xmm14, 128(%rsp)\n  movdqu %xmm15, 144(%rsp)\n"
    "  movq %rcx, g_state(%rip)\n  movq %rdx, g_code(%rip)\n"
    "  ldmxcsr 648(%rcx)\n"
    "  vmovdqu 128(%rcx), %ymm0\n  vmovdqu 160(%rcx), %ymm1\n  vmovdqu 192(%rcx), %ymm2\n  vmovdqu 224(%rcx), %ymm3\n"
    "  vmovdqu 256(%rcx), %ymm4\n  vmovdqu 288(%rcx), %ymm5\n  vmovdqu 320(%rcx), %ymm6\n  vmovdqu 352(%rcx), %ymm7\n"
    "  vmovdqu 384(%rcx), %ymm8\n  vmovdqu 416(%rcx), %ymm9\n  vmovdqu 448(%rcx), %ymm10\n  vmovdqu 480(%rcx), %ymm11\n"
    "  vmovdqu 512(%rcx), %ymm12\n  vmovdqu 544(%rcx), %ymm13\n  vmovdqu 576(%rcx), %ymm14\n  vmovdqu 608(%rcx), %ymm15\n"
    "  leaq 704+128(%rcx), %r15\n"
    "  pushq 640(%rcx)\n  popfq\n"
    "  movq 0(%rcx), %rax\n  movq 16(%rcx), %rdx\n  movq 24(%rcx), %rbx\n  movq 40(%rcx), %rbp\n"
    "  movq 48(%rcx), %rsi\n  movq 56(%rcx), %rdi\n  movq 64(%rcx), %r8\n  movq 72(%rcx), %r9\n"
    "  movq 80(%rcx), %r10\n  movq 88(%rcx), %r11\n  movq 96(%rcx), %r12\n  movq 104(%rcx), %r13\n"
    "  movq 112(%rcx), %r14\n  movq 8(%rcx), %rcx\n"
    "  call *g_code(%rip)\n"
    ".globl run_test_after\nrun_test_after:\n"
    "  pushfq\n"
    "  leaq -(704+128)(%r15), %r15\n"
    "  popq 640(%r15)\n"
    "  movq %rax, 0(%r15)\n  movq %rcx, 8(%r15)\n  movq %rdx, 16(%r15)\n  movq %rbx, 24(%r15)\n"
    "  movq %rbp, 40(%r15)\n  movq %rsi, 48(%r15)\n  movq %rdi, 56(%r15)\n  movq %r8, 64(%r15)\n"
    "  movq %r9, 72(%r15)\n  movq %r10, 80(%r15)\n  movq %r11, 88(%r15)\n  movq %r12, 96(%r15)\n"
    "  movq %r13, 104(%r15)\n  movq %r14, 112(%r15)\n"
    "  vmovdqu %ymm0, 128(%r15)\n  vmovdqu %ymm1, 160(%r15)\n  vmovdqu %ymm2, 192(%r15)\n  vmovdqu %ymm3, 224(%r15)\n"
    "  vmovdqu %ymm4, 256(%r15)\n  vmovdqu %ymm5, 288(%r15)\n  vmovdqu %ymm6, 320(%r15)\n  vmovdqu %ymm7, 352(%r15)\n"
    "  vmovdqu %ymm8, 384(%r15)\n  vmovdqu %ymm9, 416(%r15)\n  vmovdqu %ymm10, 448(%r15)\n  vmovdqu %ymm11, 480(%r15)\n"
    "  vmovdqu %ymm12, 512(%r15)\n  vmovdqu %ymm13, 544(%r15)\n  vmovdqu %ymm14, 576(%r15)\n  vmovdqu %ymm15, 608(%r15)\n"
    "  stmxcsr 648(%r15)\n"
    "  vzeroupper\n"
    "  ldmxcsr mxcsr_default(%rip)\n"
    "  movdqu 0(%rsp), %xmm6\n  movdqu 16(%rsp), %xmm7\n  movdqu 32(%rsp), %xmm8\n  movdqu 48(%rsp), %xmm9\n"
    "  movdqu 64(%rsp), %xmm10\n  movdqu 80(%rsp), %xmm11\n  movdqu 96(%rsp), %xmm12\n  movdqu 112(%rsp), %xmm13\n"
    "  movdqu 128(%rsp), %xmm14\n  movdqu 144(%rsp), %xmm15\n"
    "  addq $168, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rsi\n  popq %rdi\n  popq %rbp\n  popq %rbx\n"
    "  ret\n");
uint32_t mxcsr_default = 0x1f80;

_Static_assert(offsetof(struct state, ymm) == OFF_YMM, "layout");
_Static_assert(offsetof(struct state, flags) == OFF_FLAGS, "layout");
_Static_assert(offsetof(struct state, mxcsr) == OFF_MXCSR, "layout");
_Static_assert(offsetof(struct state, mem) == 704, "layout");

/* ------------------------------------------------------------------ inputs */

static uint64_t rng_state;
static uint64_t rng(void)
{
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static const uint32_t special_f32[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000,
    0x7f800001, 0x7fa00000, 0x00000001, 0x80000001, 0x007fffff, 0x00800000, 0x7f7fffff, 0xff7fffff,
    0x4f000000, 0xcf000000, 0x4effffff, 0x5f000000, 0xdf000000, 0x3effffff, 0x3f000000, 0x3fc00000,
    0x40200000, 0xbfc00000, 0x47800000, 0x477fe000, 0x477ff000, 0x33800000, 0x387fc000, 0x38800000,
};
static const uint64_t special_f64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull, 0x7ff0000000000000ull,
    0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff0000000000001ull, 0x0000000000000001ull,
    0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull, 0x41e0000000000000ull,
    0xc1e0000000000000ull, 0x43e0000000000000ull, 0xc3e0000000000000ull, 0x3fe0000000000000ull,
};
static const uint64_t special_int[] = {
    0, 1, 2, 31, 32, 63, 64, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000ull, 0xffffffffull,
    0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0xfffffffffffffffeull,
    0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull, 0x0123456789abcdefull, 0xffffffff80000000ull,
};

static void fill_bytes(uint8_t *p, size_t n, int style)
{
    size_t i;

    for (i = 0; i < n; i += 8)
    {
        uint64_t r = rng(), v;

        switch (style)
        {
        case 0: v = r; break;                                                   /* raw bits */
        case 1:                                                                  /* f32 specials + normals */
        {
            uint32_t a = (r & 3) ? special_f32[(r >> 2) % 32] : (uint32_t)(r >> 32);
            uint32_t b = (r & 0x30) ? special_f32[(r >> 8) % 32] : (uint32_t)(r >> 16) ^ 0x3f000000;
            v = a | (uint64_t)b << 32;
            break;
        }
        case 2: v = (r & 3) ? special_f64[(r >> 2) % 16] : r; break;              /* f64 */
        case 3:                                                                   /* ordinary floats */
        {
            float a = (float)((int)(r % 2001) - 1000) / 37.0f, b = (float)((int)((r >> 16) % 2001) - 1000) / 13.0f;
            uint32_t ua, ub;

            memcpy(&ua, &a, 4);
            memcpy(&ub, &b, 4);
            v = ua | (uint64_t)ub << 32;
            break;
        }
        case 4: v = special_int[r % 24] ^ ((r >> 8) & 1 ? 0 : (r >> 16) & 0xff); break;   /* integers */
        default: v = (r & 0x0f0f0f0f0f0f0f0full) * ((r >> 60) & 1 ? 1 : 0x11); break;     /* small bytes */
        }
        memcpy(p + i, &v, n - i < 8 ? n - i : 8);
    }
}

static void make_input(struct state *s, unsigned int test, unsigned int k, unsigned int env)
{
    unsigned int i;

    memset(s, 0, sizeof(*s));
    rng_state = 0x1234567ull * (test + 1) + 0x9e37ull * k;
    for (i = 0; i < 16; i++)
    {
        uint64_t r = rng();

        switch ((k + i) % 4)
        {
        case 0: s->gpr[i] = r; break;
        case 1: s->gpr[i] = r % 70; break;
        case 2: s->gpr[i] = special_int[r % 24]; break;
        default: s->gpr[i] = (uint32_t)r; break;
        }
    }
    for (i = 0; i < 16; i++)
        fill_bytes(s->ymm[i], 32, (k + i) % 6);
    fill_bytes(s->mem, MEM_SIZE, k % 6);
    /* arithmetic flags only: CF PF AF ZF SF OF (never TF/DF/AC) */
    s->flags = 0x202 | (rng() & 0x8d5);
    s->mxcsr = env == 0 ? 0x1f80 : 0x9fc0;      /* default, or FTZ+DAZ like many games */
}

/* ---------------------------------------------------------------- running */

static volatile LONG g_fault;
static uint8_t *g_exec;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    uint8_t *ip = (uint8_t *)e->ContextRecord->Rip;

    if (ip >= g_exec && ip < g_exec + sizeof(test_code))
    {
        unsigned int off = (unsigned int)(ip - g_exec), t = 0;

        g_fault = (LONG)e->ExceptionRecord->ExceptionCode;
        /* continue at the test's ret (the last byte of its code) */
        while (t + 1 < NTESTS && test_off[t + 1] <= off)
            t++;
        e->ContextRecord->Rip = (DWORD64)(g_exec + test_off[t + 1] - 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct result { uint64_t data; uint16_t flags; uint16_t mxcsr; uint32_t fault; };

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;

    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static struct state *g_s;
#define FAULT_HANG 0x484e4147u
static struct result *g_res;
static volatile LONG g_next, g_current = -1, g_hangs;
static void run_one(unsigned int t, unsigned int k, unsigned int env, struct state *in, struct result *r);

static DWORD WINAPI worker_main(void *arg)
{
    unsigned int t, k, env;

    for (t = (unsigned int)g_next; t < NTESTS; t++)
    {
        g_current = (LONG)t;
        if (getenv("FUZZ_TRACE"))
        {
            fprintf(stderr, "test %u %s\n", t, form_text[t]);
            fflush(stderr);
        }
        for (env = 0; env < NENVS; env++)
            for (k = 0; k < NINPUTS; k++)
                if (!test_dropped[t])
                    run_one(t, k, env, NULL, &g_res[((size_t)t * NENVS + env) * NINPUTS + k]);
    }
    g_next = NTESTS;
    return 0;
}

static void run_one(unsigned int t, unsigned int k, unsigned int env, struct state *in, struct result *r)
{
    make_input(g_s, t, k, env);
    if (in)
        *in = *g_s;
    g_fault = 0;
    run_test(g_s, g_exec + test_off[t]);
    g_s->gpr[4] = g_s->gpr[15] = 0;
    r->data = fnv(fnv(fnv(0xcbf29ce484222325ull, g_s->gpr, sizeof(g_s->gpr)), g_s->ymm, sizeof(g_s->ymm)),
            g_s->mem, sizeof(g_s->mem));
    r->flags = (uint16_t)(g_s->flags & 0x8d5);
    r->mxcsr = (uint16_t)(g_s->mxcsr & 0x3f);
    r->fault = (uint32_t)g_fault;
}

static void dump_state(FILE *f, const char *what, const struct state *s)
{
    unsigned int i, j;
    static const char *const names[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };

    fprintf(f, "      %s: flags=%03llx mxcsr=%04x\n        ", what, (unsigned long long)(s->flags & 0x8d5), s->mxcsr);
    for (i = 0; i < 16; i++)
        if (i != 4 && i != 15)
            fprintf(f, "%s=%016llx%s", names[i], (unsigned long long)s->gpr[i], i % 4 == 3 ? "\n        " : " ");
    fprintf(f, "\n");
    for (i = 0; i < 16; i++)
    {
        fprintf(f, "        ymm%-2u=", i);
        for (j = 32; j-- > 0;)
            fprintf(f, "%02x", s->ymm[i][j]);
        fprintf(f, "\n");
    }
    fprintf(f, "        mem[192..224]=");
    for (j = 0; j < 32; j++)
        fprintf(f, "%02x", s->mem[192 + j]);
    fprintf(f, "\n");
}

int main(int argc, char **argv)
{
    int generate = argc >= 3 && !strcmp(argv[1], "generate");
    unsigned int t, k, env, data_bad = 0, flag_bad = 0, mx_bad = 0, fault_bad = 0, total = 0;
    size_t n = (size_t)NTESTS * NINPUTS * NENVS;
    struct result *res = calloc(n, sizeof(*res)), *gold;
    FILE *f, *rep = NULL;

    if (argc < 3 || (!generate && (argc < 4 || strcmp(argv[1], "check"))))
    {
        printf("usage: %s generate golden.bin | check golden.bin report.txt\n", argv[0]);
        return 2;
    }
    g_exec = VirtualAlloc(NULL, sizeof(test_code) + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(g_exec, test_code, sizeof(test_code));
    if (getenv("FUZZ_SIMULATE_HANG"))       /* self-test of the watchdog: test N spins forever */
    {
        unsigned int h = (unsigned int)atoi(getenv("FUZZ_SIMULATE_HANG"));

        if (h < NTESTS && test_off[h + 1] - test_off[h] >= 2)
        {
            g_exec[test_off[h]] = 0xeb;
            g_exec[test_off[h] + 1] = 0xfe;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), g_exec, sizeof(test_code));
    /* fixed address, so pointer-like results (lea) don't depend on where memory lands */
    g_s = VirtualAlloc((void *)0x10000000, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_s)
        g_s = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    AddVectoredExceptionHandler(1, on_fault);

    /* A worker thread runs the tests; if one instruction doesn't come back within a few
     * seconds (seen under Rosetta), it is marked HANG and a new worker takes over. */
    g_res = res;
    g_next = 0;
    while (g_next < NTESTS)
    {
        HANDLE worker = CreateThread(NULL, 1 << 20, worker_main, NULL, 0, NULL);
        LONG last = -1;
        DWORD since = GetTickCount();

        for (;;)
        {
            if (WaitForSingleObject(worker, 200) == WAIT_OBJECT_0)
                break;
            if (g_current != last)
            {
                last = g_current;
                since = GetTickCount();
            }
            else if (GetTickCount() - since > 5000 && last >= 0)
            {
                size_t i;

                TerminateThread(worker, 1);
                WaitForSingleObject(worker, 2000);
                for (i = 0; i < (size_t)NENVS * NINPUTS; i++)
                    res[(size_t)last * NENVS * NINPUTS + i].fault = FAULT_HANG;
                fprintf(stderr, "test %ld (%s) hangs, skipping it\n", (long)last, form_text[last]);
                g_hangs++;
                g_next = last + 1;
                break;
            }
        }
        CloseHandle(worker);
        if (g_hangs > 50)
            break;
    }

    if (generate)
    {
        if (!(f = fopen(argv[2], "wb")) || fwrite(res, sizeof(*res), n, f) != n)
            return printf("can't write %s\n", argv[2]), 1;
        fclose(f);
        printf("wrote %u tests x %u inputs x %u environments; state block at %p\n", NTESTS, NINPUTS, NENVS, (void *)g_s);
        return 0;
    }
    gold = calloc(n, sizeof(*gold));
    if (!(f = fopen(argv[2], "rb")) || fread(gold, sizeof(*gold), n, f) != n)
        return printf("can't read %s (built from the same tests?)\n", argv[2]), 1;
    fclose(f);
    rep = fopen(argv[3], "w");
    fprintf(rep, "Rosetta check of the game's instruction forms (%u forms x %u inputs x %u environments)\n"
            "state block at %p\n\n", NTESTS, NINPUTS, NENVS, (void *)g_s);
    for (t = 0; t < NTESTS; t++)
    {
        unsigned int d = 0, fl = 0, mx = 0, fa = 0, shown = 0;

        if (test_dropped[t])
            continue;
        for (env = 0; env < NENVS; env++)
            for (k = 0; k < NINPUTS; k++)
            {
                size_t i = ((size_t)t * NENVS + env) * NINPUTS + k;
                const struct result *a = &res[i], *b = &gold[i];

                if (a->fault != b->fault)
                    fa++;
                else if (a->data != b->data)
                {
                    d++;
                    if (shown++ < 2)
                    {
                        struct state in;
                        struct result r;

                        run_one(t, k, env, &in, &r);
                        fprintf(rep, "    test %u (%s) input %u env %u: DATA differs\n", t, form_text[t], k, env);
                        dump_state(rep, "input", &in);
                        dump_state(rep, "this Mac's output", g_s);
                    }
                }
                else if (a->flags != b->flags)
                    fl++;
                else if (a->mxcsr != b->mxcsr)
                    mx++;
            }
        if (res[(size_t)t * NENVS * NINPUTS].fault == FAULT_HANG)
            fprintf(rep, "HANG test %u: %s (game uses it %ux): didn't finish within 5 s\n", t, form_text[t], form_count[t]);
        else if (d || fl || mx || fa)
            fprintf(rep, "MISMATCH test %u: %-48s game uses it %ux | data %u, flags %u, mxcsr %u, faults %u (of %u)\n",
                    t, form_text[t], form_count[t], d, fl, mx, fa, NINPUTS * NENVS);
        data_bad += d != 0;
        flag_bad += !d && fl != 0;
        mx_bad += !d && !fl && mx != 0;
        fault_bad += fa != 0;
        total++;
    }
    fprintf(rep, "\nRESULT: %u forms checked; %u give different DATA, %u differ only in flags, %u only in MXCSR status, "
            "%u fault differently\n", total, data_bad, flag_bad, mx_bad, fault_bad);
    fclose(rep);
    printf("RESULT: %u forms checked; %u give different DATA, %u differ only in flags, %u only in MXCSR status, "
            "%u fault differently\n", total, data_bad, flag_bad, mx_bad, fault_bad);
    return 0;
}
