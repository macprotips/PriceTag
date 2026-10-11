/* Rosetta instruction conformance test.
 *
 * Spider-Man 2 requires AVX2, FMA and F16C, which Rosetta 2 translates instruction by
 * instruction. This program runs a few hundred vector/scalar/atomic tests on
 * deterministic pseudo-random inputs and hashes the results.
 *
 *   rosetta_test.exe generate golden.txt   (run on a real x86 CPU: writes reference)
 *   rosetta_test.exe check golden.txt      (run under Rosetta: compares, writes report)
 *
 * The same binary does both, so compiler choices can't cause differences. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ inputs */

static uint64_t rng_state;
static uint64_t rng(void)
{
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

enum category { CAT_NORMAL, CAT_SPECIAL, CAT_BITS, CAT_COUNT };
static const char *cat_names[] = {"normal", "special", "rawbits"};

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
static const uint32_t special_i32[] = {0, 1, 0xffffffff, 0x7fffffff, 0x80000000, 0x80000001, 0xff, 0x100,
    0x7fff, 0x8000, 0xffff, 0x10000, 0x55555555, 0xaaaaaaaa, 31, 32};

/* Fills 32 bytes of one operand for a category. Lanes are filled as floats, doubles or
 * ints depending on `kind` (0 = f32, 1 = f64, 2 = int). */
static void fill(uint8_t *dst, int kind, enum category cat)
{
    int i;

    for (i = 0; i < 8; i++)
    {
        uint64_t r = rng();
        uint32_t v32;

        if (kind == 1 && i >= 4)
            break;
        if (kind == 0)
        {
            if (cat == CAT_NORMAL)
            {
                float f = (float)((int64_t)(r >> 11) - (1ll << 52)) / (float)(1ull << (r & 63 ? (r % 48) : 1));
                memcpy(&v32, &f, 4);
            }
            else if (cat == CAT_SPECIAL)
                v32 = (r & 3) ? special_f32[r % (sizeof(special_f32) / 4)] : (uint32_t)(r >> 32);
            else
                v32 = (uint32_t)r;
            memcpy(dst + i * 4, &v32, 4);
        }
        else if (kind == 1)
        {
            uint64_t v64;
            if (cat == CAT_NORMAL)
            {
                double d = (double)((int64_t)r) / (double)(1ull << (r % 60));
                memcpy(&v64, &d, 8);
            }
            else if (cat == CAT_SPECIAL)
                v64 = (r & 3) ? special_f64[r % (sizeof(special_f64) / 8)] : rng();
            else
                v64 = r;
            memcpy(dst + i * 8, &v64, 8);
        }
        else
        {
            if (cat == CAT_SPECIAL)
                v32 = special_i32[r % 16] ^ ((r >> 40) & 1 ? 0 : (uint32_t)(r >> 8) & 0x3);
            else if (cat == CAT_NORMAL)
                v32 = (uint32_t)(r % 2001) - 1000;
            else
                v32 = (uint32_t)r;
            memcpy(dst + i * 4, &v32, 4);
        }
    }
}

/* ------------------------------------------------------------------- tests */

typedef void (*test_fn)(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *out);

struct test
{
    const char *name;
    const char *insn;   /* x86 mnemonic(s), to match against the game's instruction census */
    int kind;           /* operand fill: 0 f32, 1 f64, 2 int */
    test_fn fn;
};

#define LPS(p) _mm256_loadu_ps((const float *)(p))
#define LPD(p) _mm256_loadu_pd((const double *)(p))
#define LSI(p) _mm256_loadu_si256((const __m256i *)(p))
#define L4PS(p) _mm_loadu_ps((const float *)(p))
#define L2PD(p) _mm_loadu_pd((const double *)(p))
#define L1SI(p) _mm_loadu_si128((const __m128i *)(p))
#define SPS(o, v) _mm256_storeu_ps((float *)(o), v)
#define SPD(o, v) _mm256_storeu_pd((double *)(o), v)
#define SSI(o, v) _mm256_storeu_si256((__m256i *)(o), v)
#define S4PS(o, v) _mm_storeu_ps((float *)(o), v)
#define S1SI(o, v) _mm_storeu_si128((__m128i *)(o), v)

#define T(id, expr) static void id(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o) { (void)a; (void)b; (void)c; expr; }

/* FMA */
T(t_fmadd_ps, SPS(o, _mm256_fmadd_ps(LPS(a), LPS(b), LPS(c))))
T(t_fmsub_ps, SPS(o, _mm256_fmsub_ps(LPS(a), LPS(b), LPS(c))))
T(t_fnmadd_ps, SPS(o, _mm256_fnmadd_ps(LPS(a), LPS(b), LPS(c))))
T(t_fnmsub_ps, SPS(o, _mm256_fnmsub_ps(LPS(a), LPS(b), LPS(c))))
T(t_fmaddsub_ps, SPS(o, _mm256_fmaddsub_ps(LPS(a), LPS(b), LPS(c))))
T(t_fmsubadd_ps, SPS(o, _mm256_fmsubadd_ps(LPS(a), LPS(b), LPS(c))))
T(t_fmadd_ps128, S4PS(o, _mm_fmadd_ps(L4PS(a), L4PS(b), L4PS(c))))
T(t_fmadd_ss, S4PS(o, _mm_fmadd_ss(L4PS(a), L4PS(b), L4PS(c))))
T(t_fnmadd_ss, S4PS(o, _mm_fnmadd_ss(L4PS(a), L4PS(b), L4PS(c))))
T(t_fmadd_pd, SPD(o, _mm256_fmadd_pd(LPD(a), LPD(b), LPD(c))))
T(t_fmadd_sd, _mm_storeu_pd((double *)o, _mm_fmadd_sd(L2PD(a), L2PD(b), L2PD(c))))
/* F16C */
T(t_cvtph_ps256, SPS(o, _mm256_cvtph_ps(L1SI(a))))
T(t_cvtph_ps128, S4PS(o, _mm_cvtph_ps(L1SI(a))))
T(t_cvtps_ph_rn, S1SI(o, _mm256_cvtps_ph(LPS(a), 0)))
T(t_cvtps_ph_rd, S1SI(o, _mm256_cvtps_ph(LPS(a), 1)))
T(t_cvtps_ph_ru, S1SI(o, _mm256_cvtps_ph(LPS(a), 2)))
T(t_cvtps_ph_rz, S1SI(o, _mm256_cvtps_ph(LPS(a), 3)))
T(t_cvtps_ph_mx, S1SI(o, _mm256_cvtps_ph(LPS(a), 4)))
T(t_cvtps_ph128, _mm_storel_epi64((__m128i *)o, _mm_cvtps_ph(L4PS(a), 0)))
/* AVX float arithmetic */
T(t_add_ps, SPS(o, _mm256_add_ps(LPS(a), LPS(b))))
T(t_sub_ps, SPS(o, _mm256_sub_ps(LPS(a), LPS(b))))
T(t_mul_ps, SPS(o, _mm256_mul_ps(LPS(a), LPS(b))))
T(t_div_ps, SPS(o, _mm256_div_ps(LPS(a), LPS(b))))
T(t_sqrt_ps, SPS(o, _mm256_sqrt_ps(LPS(a))))
T(t_min_ps, SPS(o, _mm256_min_ps(LPS(a), LPS(b))))
T(t_max_ps, SPS(o, _mm256_max_ps(LPS(a), LPS(b))))
T(t_min_ss, S4PS(o, _mm_min_ss(L4PS(a), L4PS(b))))
T(t_max_ss, S4PS(o, _mm_max_ss(L4PS(a), L4PS(b))))
T(t_round_near, SPS(o, _mm256_round_ps(LPS(a), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)))
T(t_round_floor, SPS(o, _mm256_floor_ps(LPS(a))))
T(t_round_ceil, SPS(o, _mm256_ceil_ps(LPS(a))))
T(t_round_trunc, SPS(o, _mm256_round_ps(LPS(a), _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)))
T(t_round_cur, SPS(o, _mm256_round_ps(LPS(a), _MM_FROUND_CUR_DIRECTION)))
T(t_round_ss, S4PS(o, _mm_round_ss(L4PS(a), L4PS(b), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)))
T(t_dp_ps_ff, SPS(o, _mm256_dp_ps(LPS(a), LPS(b), 0xff)))
T(t_dp_ps_71, SPS(o, _mm256_dp_ps(LPS(a), LPS(b), 0x71)))
T(t_dp_ps128_7f, S4PS(o, _mm_dp_ps(L4PS(a), L4PS(b), 0x7f)))
T(t_hadd_ps, SPS(o, _mm256_hadd_ps(LPS(a), LPS(b))))
T(t_hsub_ps, SPS(o, _mm256_hsub_ps(LPS(a), LPS(b))))
T(t_addsub_ps, SPS(o, _mm256_addsub_ps(LPS(a), LPS(b))))
T(t_blendv_ps, SPS(o, _mm256_blendv_ps(LPS(a), LPS(b), LPS(c))))
T(t_blend_ps, SPS(o, _mm256_blend_ps(LPS(a), LPS(b), 0xa5)))
T(t_and_ps, SPS(o, _mm256_and_ps(LPS(a), LPS(b))))
T(t_andnot_ps, SPS(o, _mm256_andnot_ps(LPS(a), LPS(b))))
T(t_xor_ps, SPS(o, _mm256_xor_ps(LPS(a), LPS(b))))
T(t_rcp_ps, SPS(o, _mm256_rcp_ps(LPS(a))))
T(t_rsqrt_ps, SPS(o, _mm256_rsqrt_ps(LPS(a))))
T(t_rcp_ss, S4PS(o, _mm_rcp_ss(L4PS(a))))
T(t_rsqrt_ss, S4PS(o, _mm_rsqrt_ss(L4PS(a))))
#define CMPT(id, pred) T(id, SPS(o, _mm256_cmp_ps(LPS(a), LPS(b), pred)))
CMPT(t_cmp_eq_oq, _CMP_EQ_OQ) CMPT(t_cmp_lt_os, _CMP_LT_OS) CMPT(t_cmp_le_os, _CMP_LE_OS)
CMPT(t_cmp_unord, _CMP_UNORD_Q) CMPT(t_cmp_neq_uq, _CMP_NEQ_UQ) CMPT(t_cmp_nlt_us, _CMP_NLT_US)
CMPT(t_cmp_nle_us, _CMP_NLE_US) CMPT(t_cmp_ord, _CMP_ORD_Q) CMPT(t_cmp_ge_oq, _CMP_GE_OQ)
CMPT(t_cmp_gt_oq, _CMP_GT_OQ) CMPT(t_cmp_neq_oq, _CMP_NEQ_OQ) CMPT(t_cmp_true, _CMP_TRUE_UQ)
T(t_movmsk_ps, { int m = _mm256_movemask_ps(LPS(a)); memcpy(o, &m, 4); })
T(t_testz_ps, { int m = _mm256_testz_ps(LPS(a), LPS(b)) | (_mm256_testc_ps(LPS(a), LPS(b)) << 1); memcpy(o, &m, 4); })
T(t_cvtps_dq, SSI(o, _mm256_cvtps_epi32(LPS(a))))
T(t_cvttps_dq, SSI(o, _mm256_cvttps_epi32(LPS(a))))
T(t_cvtdq_ps, SPS(o, _mm256_cvtepi32_ps(LSI(a))))
T(t_cvtpd_ps, S4PS(o, _mm256_cvtpd_ps(LPD(a))))
T(t_cvtps_pd, SPD(o, _mm256_cvtps_pd(L4PS(a))))
T(t_cvttpd_dq, S1SI(o, _mm256_cvttpd_epi32(LPD(a))))
T(t_cvtpd_dq, S1SI(o, _mm256_cvtpd_epi32(LPD(a))))
T(t_cvtss_si32, { int v = _mm_cvtss_si32(L4PS(a)); memcpy(o, &v, 4); })
T(t_cvttss_si32, { int v = _mm_cvttss_si32(L4PS(a)); memcpy(o, &v, 4); })
T(t_cvtss_si64, { long long v = _mm_cvtss_si64(L4PS(a)); memcpy(o, &v, 8); })
T(t_cvttss_si64, { long long v = _mm_cvttss_si64(L4PS(a)); memcpy(o, &v, 8); })
T(t_cvtsd_si32, { int v = _mm_cvtsd_si32(L2PD(a)); memcpy(o, &v, 4); })
T(t_cvttsd_si64, { long long v = _mm_cvttsd_si64(L2PD(a)); memcpy(o, &v, 8); })
T(t_cvtsi64_ss, S4PS(o, _mm_cvtsi64_ss(L4PS(b), *(const long long *)a)))
T(t_cvtsd_ss, S4PS(o, _mm_cvtsd_ss(L4PS(b), L2PD(a))))
T(t_shuffle_ps, SPS(o, _mm256_shuffle_ps(LPS(a), LPS(b), 0x1b)))
T(t_permute_ps, SPS(o, _mm256_permute_ps(LPS(a), 0x4e)))
T(t_permutevar_ps, SPS(o, _mm256_permutevar_ps(LPS(a), LSI(b))))
T(t_permute2f128, SPS(o, _mm256_permute2f128_ps(LPS(a), LPS(b), 0x21)))
T(t_permute2f128_z, SPS(o, _mm256_permute2f128_ps(LPS(a), LPS(b), 0x83)))
T(t_unpacklo_ps, SPS(o, _mm256_unpacklo_ps(LPS(a), LPS(b))))
T(t_unpackhi_ps, SPS(o, _mm256_unpackhi_ps(LPS(a), LPS(b))))
T(t_movehdup, SPS(o, _mm256_movehdup_ps(LPS(a))))
T(t_moveldup, SPS(o, _mm256_moveldup_ps(LPS(a))))
T(t_insertf128, SPS(o, _mm256_insertf128_ps(LPS(a), L4PS(b), 1)))
T(t_extractf128, S4PS(o, _mm256_extractf128_ps(LPS(a), 1)))
T(t_broadcast_ss, SPS(o, _mm256_broadcast_ss((const float *)a)))
T(t_maskload_ps, SPS(o, _mm256_maskload_ps((const float *)a, LSI(b))))
T(t_maskstore_ps, { memset(o, 0x5a, 32); _mm256_maskstore_ps((float *)o, LSI(b), LPS(a)); })
T(t_insertps, S4PS(o, _mm_insert_ps(L4PS(a), L4PS(b), 0x9c)))
T(t_div_pd, SPD(o, _mm256_div_pd(LPD(a), LPD(b))))
T(t_sqrt_pd, SPD(o, _mm256_sqrt_pd(LPD(a))))
T(t_min_pd, SPD(o, _mm256_min_pd(LPD(a), LPD(b))))
T(t_max_pd, SPD(o, _mm256_max_pd(LPD(a), LPD(b))))
T(t_round_pd, SPD(o, _mm256_round_pd(LPD(a), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)))
/* SSE / SSE4 that game math commonly uses */
T(t_sse_sqrt_ss, S4PS(o, _mm_sqrt_ss(L4PS(a))))
T(t_sse_div_ps, S4PS(o, _mm_div_ps(L4PS(a), L4PS(b))))
T(t_sse_min_ps, S4PS(o, _mm_min_ps(L4PS(a), L4PS(b))))
T(t_sse_max_ps, S4PS(o, _mm_max_ps(L4PS(a), L4PS(b))))
T(t_sse_cvtps_dq, S1SI(o, _mm_cvtps_epi32(L4PS(a))))
T(t_sse_cvttps_dq, S1SI(o, _mm_cvttps_epi32(L4PS(a))))
T(t_sse_round_ps, S4PS(o, _mm_round_ps(L4PS(a), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)))
T(t_sse_blendv_ps, S4PS(o, _mm_blendv_ps(L4PS(a), L4PS(b), L4PS(c))))
T(t_sse_ptest, { int m = _mm_testz_si128(L1SI(a), L1SI(b)) | (_mm_testc_si128(L1SI(a), L1SI(b)) << 1); memcpy(o, &m, 4); })
T(t_sse_comiss, { int m = _mm_comilt_ss(L4PS(a), L4PS(b)) | (_mm_comieq_ss(L4PS(a), L4PS(b)) << 1) | (_mm_ucomigt_ss(L4PS(a), L4PS(b)) << 2) | (_mm_comineq_ss(L4PS(a), L4PS(b)) << 3); memcpy(o, &m, 4); })
T(t_sse_crc32, { uint64_t v = _mm_crc32_u64(*(const uint64_t *)a, *(const uint64_t *)b); uint32_t w = _mm_crc32_u8((uint32_t)v, *c); memcpy(o, &v, 8); memcpy(o + 8, &w, 4); })
T(t_sse_pcmpistri, { int r = _mm_cmpistri(L1SI(a), L1SI(b), _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY); int r2 = _mm_cmpistri(L1SI(a), L1SI(b), _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ORDERED); memcpy(o, &r, 4); memcpy(o + 4, &r2, 4); })
T(t_sse_cvtsi2sd, _mm_storeu_pd((double *)o, _mm_cvtsi64_sd(L2PD(b), *(const long long *)a)))
/* AVX2 integer */
T(t_add_epi8, SSI(o, _mm256_add_epi8(LSI(a), LSI(b)))) T(t_add_epi16, SSI(o, _mm256_add_epi16(LSI(a), LSI(b))))
T(t_add_epi32, SSI(o, _mm256_add_epi32(LSI(a), LSI(b)))) T(t_add_epi64, SSI(o, _mm256_add_epi64(LSI(a), LSI(b))))
T(t_sub_epi32, SSI(o, _mm256_sub_epi32(LSI(a), LSI(b)))) T(t_sub_epi64, SSI(o, _mm256_sub_epi64(LSI(a), LSI(b))))
T(t_adds_epi8, SSI(o, _mm256_adds_epi8(LSI(a), LSI(b)))) T(t_adds_epu16, SSI(o, _mm256_adds_epu16(LSI(a), LSI(b))))
T(t_subs_epi16, SSI(o, _mm256_subs_epi16(LSI(a), LSI(b)))) T(t_subs_epu8, SSI(o, _mm256_subs_epu8(LSI(a), LSI(b))))
T(t_mullo_epi16, SSI(o, _mm256_mullo_epi16(LSI(a), LSI(b)))) T(t_mullo_epi32, SSI(o, _mm256_mullo_epi32(LSI(a), LSI(b))))
T(t_mulhi_epi16, SSI(o, _mm256_mulhi_epi16(LSI(a), LSI(b)))) T(t_mulhi_epu16, SSI(o, _mm256_mulhi_epu16(LSI(a), LSI(b))))
T(t_mulhrs_epi16, SSI(o, _mm256_mulhrs_epi16(LSI(a), LSI(b))))
T(t_mul_epi32, SSI(o, _mm256_mul_epi32(LSI(a), LSI(b)))) T(t_mul_epu32, SSI(o, _mm256_mul_epu32(LSI(a), LSI(b))))
T(t_madd_epi16, SSI(o, _mm256_madd_epi16(LSI(a), LSI(b)))) T(t_maddubs_epi16, SSI(o, _mm256_maddubs_epi16(LSI(a), LSI(b))))
T(t_sad_epu8, SSI(o, _mm256_sad_epu8(LSI(a), LSI(b))))
T(t_abs_epi8, SSI(o, _mm256_abs_epi8(LSI(a)))) T(t_abs_epi32, SSI(o, _mm256_abs_epi32(LSI(a))))
T(t_sign_epi16, SSI(o, _mm256_sign_epi16(LSI(a), LSI(b))))
T(t_min_epi32, SSI(o, _mm256_min_epi32(LSI(a), LSI(b)))) T(t_max_epu32, SSI(o, _mm256_max_epu32(LSI(a), LSI(b))))
T(t_min_epu8, SSI(o, _mm256_min_epu8(LSI(a), LSI(b)))) T(t_max_epi16, SSI(o, _mm256_max_epi16(LSI(a), LSI(b))))
T(t_cmpeq_epi8, SSI(o, _mm256_cmpeq_epi8(LSI(a), LSI(b)))) T(t_cmpgt_epi32, SSI(o, _mm256_cmpgt_epi32(LSI(a), LSI(b))))
T(t_cmpgt_epi64, SSI(o, _mm256_cmpgt_epi64(LSI(a), LSI(b)))) T(t_cmpeq_epi64, SSI(o, _mm256_cmpeq_epi64(LSI(a), LSI(b))))
T(t_avg_epu8, SSI(o, _mm256_avg_epu8(LSI(a), LSI(b))))
T(t_slli_epi32, SSI(o, _mm256_slli_epi32(LSI(a), 7))) T(t_srli_epi16, SSI(o, _mm256_srli_epi16(LSI(a), 3)))
T(t_srai_epi32, SSI(o, _mm256_srai_epi32(LSI(a), 9))) T(t_slli_epi64, SSI(o, _mm256_slli_epi64(LSI(a), 33)))
T(t_sll_epi32, SSI(o, _mm256_sll_epi32(LSI(a), _mm_cvtsi32_si128(*b & 63))))
T(t_sra_epi16, SSI(o, _mm256_sra_epi16(LSI(a), _mm_cvtsi32_si128(*b))))
T(t_sllv_epi32, SSI(o, _mm256_sllv_epi32(LSI(a), _mm256_and_si256(LSI(b), _mm256_set1_epi32(63)))))
T(t_srlv_epi32, SSI(o, _mm256_srlv_epi32(LSI(a), _mm256_and_si256(LSI(b), _mm256_set1_epi32(63)))))
T(t_srav_epi32, SSI(o, _mm256_srav_epi32(LSI(a), _mm256_and_si256(LSI(b), _mm256_set1_epi32(63)))))
T(t_sllv_epi64, SSI(o, _mm256_sllv_epi64(LSI(a), _mm256_and_si256(LSI(b), _mm256_set1_epi64x(127)))))
T(t_srlv_epi64, SSI(o, _mm256_srlv_epi64(LSI(a), _mm256_and_si256(LSI(b), _mm256_set1_epi64x(127)))))
T(t_srlv_raw, SSI(o, _mm256_srlv_epi32(LSI(a), LSI(b))))
T(t_packs_epi32, SSI(o, _mm256_packs_epi32(LSI(a), LSI(b)))) T(t_packus_epi16, SSI(o, _mm256_packus_epi16(LSI(a), LSI(b))))
T(t_packus_epi32, SSI(o, _mm256_packus_epi32(LSI(a), LSI(b))))
T(t_unpacklo_epi8, SSI(o, _mm256_unpacklo_epi8(LSI(a), LSI(b)))) T(t_unpackhi_epi32, SSI(o, _mm256_unpackhi_epi32(LSI(a), LSI(b))))
T(t_unpacklo_epi64, SSI(o, _mm256_unpacklo_epi64(LSI(a), LSI(b))))
T(t_shuffle_epi8, SSI(o, _mm256_shuffle_epi8(LSI(a), LSI(b))))
T(t_shuffle_epi32, SSI(o, _mm256_shuffle_epi32(LSI(a), 0x93)))
T(t_shufflehi_epi16, SSI(o, _mm256_shufflehi_epi16(LSI(a), 0x1b))) T(t_shufflelo_epi16, SSI(o, _mm256_shufflelo_epi16(LSI(a), 0xb1)))
T(t_alignr_epi8, SSI(o, _mm256_alignr_epi8(LSI(a), LSI(b), 5)))
T(t_blend_epi16, SSI(o, _mm256_blend_epi16(LSI(a), LSI(b), 0x5a))) T(t_blend_epi32, SSI(o, _mm256_blend_epi32(LSI(a), LSI(b), 0x3c)))
T(t_blendv_epi8, SSI(o, _mm256_blendv_epi8(LSI(a), LSI(b), LSI(c))))
T(t_permutevar8x32, SSI(o, _mm256_permutevar8x32_epi32(LSI(a), LSI(b))))
T(t_permutevar8x32_ps, SPS(o, _mm256_permutevar8x32_ps(LPS(a), LSI(b))))
T(t_permute4x64, SSI(o, _mm256_permute4x64_epi64(LSI(a), 0x4b)))
T(t_permute4x64_pd, SPD(o, _mm256_permute4x64_pd(LPD(a), 0xd8)))
T(t_permute2x128, SSI(o, _mm256_permute2x128_si256(LSI(a), LSI(b), 0x31)))
T(t_bcast_d, SSI(o, _mm256_broadcastd_epi32(L1SI(a)))) T(t_bcast_q, SSI(o, _mm256_broadcastq_epi64(L1SI(a))))
T(t_bcast_b, SSI(o, _mm256_broadcastb_epi8(L1SI(a)))) T(t_bcast_ss_reg, SPS(o, _mm256_broadcastss_ps(L4PS(a))))
T(t_bcast_si128, SSI(o, _mm256_broadcastsi128_si256(L1SI(a))))
T(t_cvtepi8_epi32, SSI(o, _mm256_cvtepi8_epi32(L1SI(a)))) T(t_cvtepu8_epi32, SSI(o, _mm256_cvtepu8_epi32(L1SI(a))))
T(t_cvtepi16_epi32, SSI(o, _mm256_cvtepi16_epi32(L1SI(a)))) T(t_cvtepu16_epi32, SSI(o, _mm256_cvtepu16_epi32(L1SI(a))))
T(t_cvtepi32_epi64, SSI(o, _mm256_cvtepi32_epi64(L1SI(a)))) T(t_cvtepu8_epi16, SSI(o, _mm256_cvtepu8_epi16(L1SI(a))))
T(t_movemask_epi8, { int m = _mm256_movemask_epi8(LSI(a)); memcpy(o, &m, 4); })
T(t_testz_si256, { int m = _mm256_testz_si256(LSI(a), LSI(b)) | (_mm256_testc_si256(LSI(a), LSI(b)) << 1) | (_mm256_testnzc_si256(LSI(a), LSI(b)) << 2); memcpy(o, &m, 4); })
T(t_hadd_epi16, SSI(o, _mm256_hadd_epi16(LSI(a), LSI(b)))) T(t_hadd_epi32, SSI(o, _mm256_hadd_epi32(LSI(a), LSI(b))))
T(t_mpsadbw, SSI(o, _mm256_mpsadbw_epu8(LSI(a), LSI(b), 0x2d)))
T(t_minpos, S1SI(o, _mm_minpos_epu16(L1SI(a))))
T(t_extracti128, S1SI(o, _mm256_extracti128_si256(LSI(a), 1)))
T(t_inserti128, SSI(o, _mm256_inserti128_si256(LSI(a), L1SI(b), 0)))
T(t_maskload_epi32, SSI(o, _mm256_maskload_epi32((const int *)a, LSI(b))))
T(t_maskstore_epi64, { memset(o, 0x5a, 32); _mm256_maskstore_epi64((long long *)o, LSI(b), LSI(a)); })
/* gathers: indices from b masked into a 64-entry table built from a/c */
static float g_ftab[64]; static int g_itab[64]; static double g_dtab[64];
static void build_tables(const uint8_t *a, const uint8_t *c)
{
    int i;
    for (i = 0; i < 64; i++)
    {
        uint32_t v = ((const uint32_t *)(i & 1 ? c : a))[(i >> 1) & 7] ^ (uint32_t)(i * 0x9e3779b9u);
        memcpy(&g_ftab[i], &v, 4);
        g_itab[i] = (int)v;
        memcpy(&g_dtab[i], &(uint64_t){((uint64_t)v << 29) ^ v}, 8);
    }
}
#define GIDX(b) _mm256_and_si256(LSI(b), _mm256_set1_epi32(63))
T(t_gather_ps4, { build_tables(a, c); SPS(o, _mm256_i32gather_ps(g_ftab, GIDX(b), 4)); })
T(t_gather_epi32, { build_tables(a, c); SSI(o, _mm256_i32gather_epi32(g_itab, GIDX(b), 4)); })
T(t_gather_ps_mask, { build_tables(a, c); SPS(o, _mm256_mask_i32gather_ps(LPS(c), g_ftab, GIDX(b), LPS(a), 4)); })
T(t_gather_pd, { build_tables(a, c); SPD(o, _mm256_i32gather_pd(g_dtab, _mm_and_si128(L1SI(b), _mm_set1_epi32(63)), 8)); })
T(t_gather_epi64, { build_tables(a, c); SSI(o, _mm256_i64gather_epi64((const long long *)g_dtab, _mm256_and_si256(LSI(b), _mm256_set1_epi64x(63)), 8)); })
T(t_gather_ps_s1, { build_tables(a, c); SPS(o, _mm256_i32gather_ps(g_ftab, _mm256_slli_epi32(GIDX(b), 2), 1)); })
/* BMI1/BMI2/LZCNT/POPCNT */
#define U64(p) (*(const uint64_t *)(p))
#define U32(p) (*(const uint32_t *)(p))
T(t_andn, { uint64_t v = _andn_u64(U64(a), U64(b)); memcpy(o, &v, 8); })
T(t_bextr, { uint64_t v = _bextr_u64(U64(a), U32(b) & 63, (U32(b) >> 8) & 127); uint32_t w = _bextr_u32(U32(a), U32(c) & 31, (U32(c) >> 8) & 63); memcpy(o, &v, 8); memcpy(o + 8, &w, 4); })
T(t_blsi, { uint64_t v = _blsi_u64(U64(a)); uint64_t w = _blsmsk_u64(U64(a)); uint64_t x = _blsr_u64(U64(a)); memcpy(o, &v, 8); memcpy(o + 8, &w, 8); memcpy(o + 16, &x, 8); })
T(t_tzcnt, { uint64_t v = _tzcnt_u64(U64(a) & U64(b)); uint32_t w = _tzcnt_u32(U32(a) & U32(c)); memcpy(o, &v, 8); memcpy(o + 8, &w, 4); })
T(t_lzcnt, { uint64_t v = _lzcnt_u64(U64(a) & U64(b)); uint32_t w = _lzcnt_u32(U32(a) & U32(c)); memcpy(o, &v, 8); memcpy(o + 8, &w, 4); })
T(t_popcnt, { uint64_t v = __popcnt64(U64(a)); memcpy(o, &v, 8); })
T(t_bzhi, { uint64_t v = _bzhi_u64(U64(a), U32(b) & 255); memcpy(o, &v, 8); })
T(t_pdep, { uint64_t v = _pdep_u64(U64(a), U64(b)); uint32_t w = _pdep_u32(U32(a), U32(c)); memcpy(o, &v, 8); memcpy(o + 8, &w, 4); })
T(t_pext, { uint64_t v = _pext_u64(U64(a), U64(b)); uint32_t w = _pext_u32(U32(a), U32(c)); memcpy(o, &v, 8); memcpy(o + 8, &w, 4); })
T(t_mulx, { unsigned long long hi; uint64_t lo = _mulx_u64(U64(a), U64(b), &hi); memcpy(o, &lo, 8); memcpy(o + 8, &hi, 8); })
static void t_shlx(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    uint64_t v, w, x, sa = U64(b), sb = U64(c), va = U64(a);

    __asm__("shlxq %2, %1, %0" : "=r"(v) : "r"(va), "r"(sa));
    __asm__("shrxq %2, %1, %0" : "=r"(w) : "r"(va), "r"(sb));
    __asm__("sarxq %2, %1, %0" : "=r"(x) : "r"(va), "r"(sa >> 3));
    memcpy(o, &v, 8); memcpy(o + 8, &w, 8); memcpy(o + 16, &x, 8);
}

static void t_rorx(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    uint64_t v, va = U64(a);
    uint32_t w, wb = U32(b);

    (void)c;
    __asm__("rorxq $13, %1, %0" : "=r"(v) : "r"(va));
    __asm__("rorxl $7, %1, %0" : "=r"(w) : "r"(wb));
    memcpy(o, &v, 8); memcpy(o + 8, &w, 4);
}
/* Exact encodings the game uses most (the compiler would pick other forms otherwise). */
static void t_asm_vfmadd231ps(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    __m256 acc = LPS(c);
    __asm__("vfmadd231ps %2, %1, %0" : "+x"(acc) : "x"(LPS(a)), "x"(LPS(b)));
    SPS(o, acc);
}
static void t_asm_vfmadd231ss(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    __m128 acc = L4PS(c);
    __asm__("vfmadd231ss %2, %1, %0" : "+x"(acc) : "x"(L4PS(a)), "x"(L4PS(b)));
    S4PS(o, acc);
}
static void t_asm_vfmaddsub231ps(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    __m256 acc = LPS(c);
    __asm__("vfmaddsub231ps %2, %1, %0" : "+x"(acc) : "x"(LPS(a)), "x"(LPS(b)));
    SPS(o, acc);
}
static void t_asm_vfnmadd231ss(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    __m128 acc = L4PS(c);
    __asm__("vfnmadd231ss %2, %1, %0" : "+x"(acc) : "x"(L4PS(a)), "x"(L4PS(b)));
    S4PS(o, acc);
}
static void t_asm_rsqrtps_sse(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    __m128 v = L4PS(a);
    (void)b; (void)c;
    __asm__("rsqrtps %0, %0" : "+x"(v));   /* legacy SSE encoding */
    S4PS(o, v);
}
static void t_asm_rcpps_sse(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    __m128 v = L4PS(a);
    (void)b; (void)c;
    __asm__("rcpps %0, %0" : "+x"(v));
    S4PS(o, v);
}
static void t_asm_andnps_sse(const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *o)
{
    __m128 v = L4PS(a);
    (void)c;
    __asm__("andnps %1, %0" : "+x"(v) : "x"(L4PS(b)));
    S4PS(o, v);
}

/* x87 transcendental-free scalar double math compilers emit for game code */
T(t_scalar_div_sd, { double v = *(const double *)a / *(const double *)b; memcpy(o, &v, 8); })
T(t_scalar_sqrt_sd, S4PS(o, _mm_castpd_ps(_mm_sqrt_sd(L2PD(b), L2PD(a)))))

static const struct test tests[] = {
#define E(id, insn, kind) {&#id[2], insn, kind, id}
    E(t_fmadd_ps, "vfmadd", 0), E(t_fmsub_ps, "vfmsub", 0), E(t_fnmadd_ps, "vfnmadd", 0), E(t_fnmsub_ps, "vfnmsub", 0),
    E(t_fmaddsub_ps, "vfmaddsub", 0), E(t_fmsubadd_ps, "vfmsubadd", 0), E(t_fmadd_ps128, "vfmadd", 0),
    E(t_fmadd_ss, "vfmadd*ss", 0), E(t_fnmadd_ss, "vfnmadd*ss", 0), E(t_fmadd_pd, "vfmadd*pd", 1), E(t_fmadd_sd, "vfmadd*sd", 1),
    E(t_cvtph_ps256, "vcvtph2ps", 2), E(t_cvtph_ps128, "vcvtph2ps", 2), E(t_cvtps_ph_rn, "vcvtps2ph", 0),
    E(t_cvtps_ph_rd, "vcvtps2ph", 0), E(t_cvtps_ph_ru, "vcvtps2ph", 0), E(t_cvtps_ph_rz, "vcvtps2ph", 0),
    E(t_cvtps_ph_mx, "vcvtps2ph", 0), E(t_cvtps_ph128, "vcvtps2ph", 0),
    E(t_add_ps, "vaddps", 0), E(t_sub_ps, "vsubps", 0), E(t_mul_ps, "vmulps", 0), E(t_div_ps, "vdivps", 0),
    E(t_sqrt_ps, "vsqrtps", 0), E(t_min_ps, "vminps", 0), E(t_max_ps, "vmaxps", 0), E(t_min_ss, "minss", 0), E(t_max_ss, "maxss", 0),
    E(t_round_near, "vroundps", 0), E(t_round_floor, "vroundps", 0), E(t_round_ceil, "vroundps", 0),
    E(t_round_trunc, "vroundps", 0), E(t_round_cur, "vroundps", 0), E(t_round_ss, "roundss", 0),
    E(t_dp_ps_ff, "vdpps", 0), E(t_dp_ps_71, "vdpps", 0), E(t_dp_ps128_7f, "dpps", 0),
    E(t_hadd_ps, "vhaddps", 0), E(t_hsub_ps, "vhsubps", 0), E(t_addsub_ps, "vaddsubps", 0),
    E(t_blendv_ps, "vblendvps", 0), E(t_blend_ps, "vblendps", 0), E(t_and_ps, "vandps", 0), E(t_andnot_ps, "vandnps", 0),
    E(t_xor_ps, "vxorps", 0), E(t_rcp_ps, "vrcpps", 0), E(t_rsqrt_ps, "vrsqrtps", 0), E(t_rcp_ss, "rcpss", 0), E(t_rsqrt_ss, "rsqrtss", 0),
    E(t_cmp_eq_oq, "vcmpps", 0), E(t_cmp_lt_os, "vcmpps", 0), E(t_cmp_le_os, "vcmpps", 0), E(t_cmp_unord, "vcmpps", 0),
    E(t_cmp_neq_uq, "vcmpps", 0), E(t_cmp_nlt_us, "vcmpps", 0), E(t_cmp_nle_us, "vcmpps", 0), E(t_cmp_ord, "vcmpps", 0),
    E(t_cmp_ge_oq, "vcmpps", 0), E(t_cmp_gt_oq, "vcmpps", 0), E(t_cmp_neq_oq, "vcmpps", 0), E(t_cmp_true, "vcmpps", 0),
    E(t_movmsk_ps, "vmovmskps", 0), E(t_testz_ps, "vtestps", 0), E(t_cvtps_dq, "vcvtps2dq", 0), E(t_cvttps_dq, "vcvttps2dq", 0),
    E(t_cvtdq_ps, "vcvtdq2ps", 2), E(t_cvtpd_ps, "vcvtpd2ps", 1), E(t_cvtps_pd, "vcvtps2pd", 0), E(t_cvttpd_dq, "vcvttpd2dq", 1),
    E(t_cvtpd_dq, "vcvtpd2dq", 1), E(t_cvtss_si32, "cvtss2si", 0), E(t_cvttss_si32, "cvttss2si", 0), E(t_cvtss_si64, "cvtss2si", 0),
    E(t_cvttss_si64, "cvttss2si", 0), E(t_cvtsd_si32, "cvtsd2si", 1), E(t_cvttsd_si64, "cvttsd2si", 1), E(t_cvtsi64_ss, "cvtsi2ss", 2),
    E(t_cvtsd_ss, "cvtsd2ss", 1), E(t_shuffle_ps, "vshufps", 0), E(t_permute_ps, "vpermilps", 0), E(t_permutevar_ps, "vpermilps", 0),
    E(t_permute2f128, "vperm2f128", 0), E(t_permute2f128_z, "vperm2f128", 0), E(t_unpacklo_ps, "vunpcklps", 0),
    E(t_unpackhi_ps, "vunpckhps", 0), E(t_movehdup, "vmovshdup", 0), E(t_moveldup, "vmovsldup", 0),
    E(t_insertf128, "vinsertf128", 0), E(t_extractf128, "vextractf128", 0), E(t_broadcast_ss, "vbroadcastss", 0),
    E(t_maskload_ps, "vmaskmovps", 0), E(t_maskstore_ps, "vmaskmovps", 0), E(t_insertps, "insertps", 0),
    E(t_div_pd, "vdivpd", 1), E(t_sqrt_pd, "vsqrtpd", 1), E(t_min_pd, "vminpd", 1), E(t_max_pd, "vmaxpd", 1), E(t_round_pd, "vroundpd", 1),
    E(t_sse_sqrt_ss, "sqrtss", 0), E(t_sse_div_ps, "divps", 0), E(t_sse_min_ps, "minps", 0), E(t_sse_max_ps, "maxps", 0),
    E(t_sse_cvtps_dq, "cvtps2dq", 0), E(t_sse_cvttps_dq, "cvttps2dq", 0), E(t_sse_round_ps, "roundps", 0),
    E(t_sse_blendv_ps, "blendvps", 0), E(t_sse_ptest, "ptest", 2), E(t_sse_comiss, "comiss", 0), E(t_sse_crc32, "crc32", 2),
    E(t_sse_pcmpistri, "pcmpistri", 2), E(t_sse_cvtsi2sd, "cvtsi2sd", 2),
    E(t_add_epi8, "vpaddb", 2), E(t_add_epi16, "vpaddw", 2), E(t_add_epi32, "vpaddd", 2), E(t_add_epi64, "vpaddq", 2),
    E(t_sub_epi32, "vpsubd", 2), E(t_sub_epi64, "vpsubq", 2), E(t_adds_epi8, "vpaddsb", 2), E(t_adds_epu16, "vpaddusw", 2),
    E(t_subs_epi16, "vpsubsw", 2), E(t_subs_epu8, "vpsubusb", 2), E(t_mullo_epi16, "vpmullw", 2), E(t_mullo_epi32, "vpmulld", 2),
    E(t_mulhi_epi16, "vpmulhw", 2), E(t_mulhi_epu16, "vpmulhuw", 2), E(t_mulhrs_epi16, "vpmulhrsw", 2),
    E(t_mul_epi32, "vpmuldq", 2), E(t_mul_epu32, "vpmuludq", 2), E(t_madd_epi16, "vpmaddwd", 2), E(t_maddubs_epi16, "vpmaddubsw", 2),
    E(t_sad_epu8, "vpsadbw", 2), E(t_abs_epi8, "vpabsb", 2), E(t_abs_epi32, "vpabsd", 2), E(t_sign_epi16, "vpsignw", 2),
    E(t_min_epi32, "vpminsd", 2), E(t_max_epu32, "vpmaxud", 2), E(t_min_epu8, "vpminub", 2), E(t_max_epi16, "vpmaxsw", 2),
    E(t_cmpeq_epi8, "vpcmpeqb", 2), E(t_cmpgt_epi32, "vpcmpgtd", 2), E(t_cmpgt_epi64, "vpcmpgtq", 2), E(t_cmpeq_epi64, "vpcmpeqq", 2),
    E(t_avg_epu8, "vpavgb", 2), E(t_slli_epi32, "vpslld", 2), E(t_srli_epi16, "vpsrlw", 2), E(t_srai_epi32, "vpsrad", 2),
    E(t_slli_epi64, "vpsllq", 2), E(t_sll_epi32, "vpslld", 2), E(t_sra_epi16, "vpsraw", 2), E(t_sllv_epi32, "vpsllvd", 2),
    E(t_srlv_epi32, "vpsrlvd", 2), E(t_srav_epi32, "vpsravd", 2), E(t_sllv_epi64, "vpsllvq", 2), E(t_srlv_epi64, "vpsrlvq", 2),
    E(t_srlv_raw, "vpsrlvd", 2), E(t_packs_epi32, "vpackssdw", 2), E(t_packus_epi16, "vpackuswb", 2), E(t_packus_epi32, "vpackusdw", 2),
    E(t_unpacklo_epi8, "vpunpcklbw", 2), E(t_unpackhi_epi32, "vpunpckhdq", 2), E(t_unpacklo_epi64, "vpunpcklqdq", 2),
    E(t_shuffle_epi8, "vpshufb", 2), E(t_shuffle_epi32, "vpshufd", 2), E(t_shufflehi_epi16, "vpshufhw", 2),
    E(t_shufflelo_epi16, "vpshuflw", 2), E(t_alignr_epi8, "vpalignr", 2), E(t_blend_epi16, "vpblendw", 2),
    E(t_blend_epi32, "vpblendd", 2), E(t_blendv_epi8, "vpblendvb", 2), E(t_permutevar8x32, "vpermd", 2),
    E(t_permutevar8x32_ps, "vpermps", 0), E(t_permute4x64, "vpermq", 2), E(t_permute4x64_pd, "vpermpd", 1),
    E(t_permute2x128, "vperm2i128", 2), E(t_bcast_d, "vpbroadcastd", 2), E(t_bcast_q, "vpbroadcastq", 2),
    E(t_bcast_b, "vpbroadcastb", 2), E(t_bcast_ss_reg, "vbroadcastss", 0), E(t_bcast_si128, "vbroadcasti128", 2),
    E(t_cvtepi8_epi32, "vpmovsxbd", 2), E(t_cvtepu8_epi32, "vpmovzxbd", 2), E(t_cvtepi16_epi32, "vpmovsxwd", 2),
    E(t_cvtepu16_epi32, "vpmovzxwd", 2), E(t_cvtepi32_epi64, "vpmovsxdq", 2), E(t_cvtepu8_epi16, "vpmovzxbw", 2),
    E(t_movemask_epi8, "vpmovmskb", 2), E(t_testz_si256, "vptest", 2), E(t_hadd_epi16, "vphaddw", 2), E(t_hadd_epi32, "vphaddd", 2),
    E(t_mpsadbw, "vmpsadbw", 2), E(t_minpos, "phminposuw", 2), E(t_extracti128, "vextracti128", 2), E(t_inserti128, "vinserti128", 2),
    E(t_maskload_epi32, "vpmaskmovd", 2), E(t_maskstore_epi64, "vpmaskmovq", 2),
    E(t_gather_ps4, "vgatherdps", 2), E(t_gather_epi32, "vpgatherdd", 2), E(t_gather_ps_mask, "vgatherdps", 2),
    E(t_gather_pd, "vgatherdpd", 2), E(t_gather_epi64, "vpgatherqq", 2), E(t_gather_ps_s1, "vgatherdps", 2),
    E(t_andn, "andn", 2), E(t_bextr, "bextr", 2), E(t_blsi, "blsi", 2), E(t_tzcnt, "tzcnt", 2), E(t_lzcnt, "lzcnt", 2),
    E(t_popcnt, "popcnt", 2), E(t_bzhi, "bzhi", 2), E(t_pdep, "pdep", 2), E(t_pext, "pext", 2), E(t_mulx, "mulx", 2),
    E(t_asm_vfmadd231ps, "vfmadd231ps", 0), E(t_asm_vfmadd231ss, "vfmadd231ss", 0),
    E(t_asm_vfmaddsub231ps, "vfmaddsub231ps", 0), E(t_asm_vfnmadd231ss, "vfnmadd231ss", 0),
    E(t_asm_rsqrtps_sse, "rsqrtps", 0), E(t_asm_rcpps_sse, "rcpps", 0), E(t_asm_andnps_sse, "andnps", 0),
    E(t_shlx, "shlx", 2), E(t_rorx, "rorx", 2), E(t_scalar_div_sd, "divsd", 1), E(t_scalar_sqrt_sd, "sqrtsd", 1),
#undef E
};
#define NTESTS (sizeof(tests) / sizeof(tests[0]))

/* MXCSR environments a game runs in: default, and flush-to-zero + denormals-are-zero
 * (which almost every engine enables), plus round-down for the "current mode" tests. */
static const struct { const char *name; unsigned int mxcsr; } envs[] = {
    {"default", 0x1f80}, {"ftz+daz", 0x1f80 | 0x8040}, {"rounddown", 0x1f80 | 0x2000},
};
#define NENVS 3
#define ITERS 4000

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    while (n--)
        h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}

/* Hash of one test in one environment and input category, plus the first output for
 * diagnostics. */
static uint64_t run_one(const struct test *t, int env, int cat, uint8_t first_in[96], uint8_t first_out[32],
        int *first_diff_iter, const uint8_t (*expect)[32])
{
    __attribute__((aligned(32))) uint8_t a[64], b[64], c[64], o[64];
    unsigned int saved = _mm_getcsr();
    uint64_t h = 0xcbf29ce484222325ull;
    int i;

    rng_state = 0x1234567ull * (uint64_t)(t - tests + 1) + (uint64_t)cat * 0x9999 + (uint64_t)env;
    for (i = 0; i < ITERS; i++)
    {
        memset(a, 0, 64); memset(b, 0, 64); memset(c, 0, 64);
        fill(a, t->kind, (enum category)cat);
        fill(b, t->kind, (enum category)cat);
        fill(c, t->kind == 1 ? 1 : t->kind, (enum category)cat);
        if (t->kind == 1) { fill(a + 32, 1, cat); }
        memset(o, 0, 64);
        _mm_setcsr(envs[env].mxcsr);
        t->fn(a, b, c, o);
        _mm_setcsr(saved);
        h = fnv(h, o, 32);
        if (i == 0)
        {
            memcpy(first_in, a, 32); memcpy(first_in + 32, b, 32); memcpy(first_in + 64, c, 32);
            memcpy(first_out, o, 32);
        }
        if (expect && *first_diff_iter < 0 && i < 64 && memcmp(o, expect[i], 32))
            *first_diff_iter = i;
    }
    return h;
}

/* ------------------------------------------- estimate instructions vs x86 spec */

/* rcpps/rsqrtps/rcpss/rsqrtss results differ bit-wise between Intel and AMD, so they are
 * checked against the documented bound instead: |relative error| <= 1.5 * 2^-12. */
static int estimate_tests(FILE *rep)
{
    const double bound = 1.5 / 4096.0;
    double worst[4] = {0};
    float worst_in[4] = {0};
    int i, k, failures = 0;
    static const char *names[] = {"rcpps (SSE)", "rsqrtps (SSE)", "vrcpps (AVX)", "vrsqrtps (AVX)"};

    rng_state = 42;
    for (i = 0; i < 200000; i++)
    {
        __attribute__((aligned(32))) float in[8], out[4][8];
        int j;

        for (j = 0; j < 8; j++)
        {
            /* positive normal floats across the whole exponent range the estimates cover */
            uint32_t bits = 0x00800000u + (uint32_t)(rng() % (0x7e800000u - 0x00800000u));
            memcpy(&in[j], &bits, 4);
        }
        {
            __m128 r = _mm_loadu_ps(in), q = _mm_loadu_ps(in);
            __asm__("rcpps %0, %0" : "+x"(r));      /* legacy encodings, as in the game */
            __asm__("rsqrtps %0, %0" : "+x"(q));
            _mm_storeu_ps(out[0], r);
            _mm_storeu_ps(out[1], q);
        }
        _mm256_storeu_ps(out[2], _mm256_rcp_ps(_mm256_loadu_ps(in)));
        _mm256_storeu_ps(out[3], _mm256_rsqrt_ps(_mm256_loadu_ps(in)));
        for (k = 0; k < 4; k++)
            for (j = 0; j < (k < 2 ? 4 : 8); j++)
            {
                double x = in[j], exact = (k & 1) ? 1.0 / sqrt(x) : 1.0 / x, err;

                if (exact == 0.0 || !isfinite(exact) || exact > 3.0e38 || exact < 1.2e-38)
                    continue;
                err = fabs(out[k][j] - exact) / exact;
                if (err > worst[k])
                {
                    worst[k] = err;
                    worst_in[k] = in[j];
                }
            }
    }
    for (k = 0; k < 4; k++)
    {
        fprintf(rep, "%-15s max relative error %.3g (x86 guarantees <= %.3g, i.e. %.1f bits) at x=%g %s\n",
                names[k], worst[k], bound, worst[k] > 0 ? -log2(worst[k]) : 99.0, worst_in[k],
                worst[k] <= bound ? "OK" : "OUT OF SPEC");
        failures += worst[k] > bound;
    }
    return failures;
}

/* ----------------------------------------------------------- threading tests */

static volatile LONG g_go;
static __attribute__((aligned(64))) volatile uint8_t g_line[256];
static volatile uint64_t g_counter64;
static __attribute__((aligned(16))) volatile uint64_t g_pair[2];
#define THREAD_ITERS 200000

struct mp_data { volatile uint64_t data; char pad[56]; volatile uint64_t flag; } __attribute__((aligned(64)));
static struct mp_data g_mp[1024];
static volatile LONG g_mp_fail;

static DWORD WINAPI th_xadd(void *p)
{
    int i;
    (void)p;
    while (!g_go) {}
    for (i = 0; i < THREAD_ITERS; i++)
    {
        uint64_t one64 = 1;
        uint32_t one32 = 1;
        /* Explicit lock xadd: the compiler would turn misaligned atomics into library calls. */
        __asm__ volatile("lock xaddq %0, %1" : "+r"(one64), "+m"(g_counter64) :: "memory");
        one64 = 1;
        __asm__ volatile("lock xaddl %0, %1" : "+r"(one32), "+m"(*(volatile uint32_t *)(g_line + 62)) :: "memory");
        __asm__ volatile("lock xaddq %0, %1" : "+r"(one64), "+m"(*(volatile uint64_t *)(g_line + 129)) :: "memory");
    }
    return 0;
}

static int cmpxchg16b(volatile uint64_t *dst, __int128 expected, __int128 desired)
{
    uint64_t lo = (uint64_t)expected, hi = (uint64_t)(expected >> 64);
    unsigned char ok;

    __asm__ volatile("lock cmpxchg16b %1\n\tsete %0"
            : "=q"(ok), "+m"(*(volatile __int128 *)dst), "+a"(lo), "+d"(hi)
            : "b"((uint64_t)desired), "c"((uint64_t)(desired >> 64))
            : "memory", "cc");
    return ok;
}

static DWORD WINAPI th_cx16(void *p)
{
    int i;
    (void)p;
    while (!g_go) {}
    for (i = 0; i < THREAD_ITERS / 4; i++)
    {
        __int128 old, nw;
        do
        {
            old = *(volatile __int128 *)g_pair;
            uint64_t lo = (uint64_t)old, hi = (uint64_t)(old >> 64);
            nw = ((__int128)(hi + 1) << 64) | (lo + 2);
        } while (!cmpxchg16b(g_pair, old, nw));
    }
    return 0;
}

/* Message passing litmus test: x86 (TSO) never lets the reader see flag=1 and data=0. */
static DWORD WINAPI th_mp_writer(void *p)
{
    int i, nt = p != NULL;
    while (!g_go) {}
    for (i = 0; i < 1024; i++)
    {
        if (nt)
        {
            _mm_stream_si64((long long *)&g_mp[i].data, 1);   /* movnti: weakly ordered... */
            _mm_sfence();                                      /* ...until this fence */
        }
        else
            g_mp[i].data = 1;
        __asm__ volatile("" ::: "memory");
        g_mp[i].flag = 1;
    }
    return 0;
}

static DWORD WINAPI th_mp_reader(void *p)
{
    int i;
    (void)p;
    while (!g_go) {}
    for (i = 0; i < 1024; i++)
    {
        while (!g_mp[i].flag) {}
        __asm__ volatile("" ::: "memory");
        if (g_mp[i].data != 1)
            InterlockedIncrement(&g_mp_fail);
    }
    return 0;
}

static void run_threads(DWORD (WINAPI *fn)(void *), int n, void *arg)
{
    HANDLE h[16];
    int i;

    g_go = 0;
    for (i = 0; i < n; i++)
        h[i] = CreateThread(NULL, 0, fn, arg, 0, NULL);
    Sleep(20);
    g_go = 1;
    WaitForMultipleObjects(n, h, TRUE, INFINITE);
    for (i = 0; i < n; i++)
        CloseHandle(h[i]);
}

static int thread_tests(FILE *rep)
{
    int failures = 0, round;
    const int threads = 6;
    uint32_t split;
    uint64_t mis;

    memset((void *)g_line, 0, sizeof(g_line));
    g_counter64 = 0;
    run_threads(th_xadd, threads, NULL);
    memcpy(&split, (const void *)(g_line + 62), 4);
    memcpy(&mis, (const void *)(g_line + 129), 8);
    fprintf(rep, "atomic lock xadd (aligned 64-bit): %llu / %u %s\n", (unsigned long long)g_counter64,
            threads * THREAD_ITERS, g_counter64 == (uint64_t)threads * THREAD_ITERS ? "OK" : "FAIL");
    fprintf(rep, "atomic lock xadd (split across cache line): %u / %u %s\n", split, threads * THREAD_ITERS,
            split == (uint32_t)threads * THREAD_ITERS ? "OK" : "FAIL");
    fprintf(rep, "atomic lock xadd (misaligned, one line): %llu / %u %s\n", (unsigned long long)mis,
            threads * THREAD_ITERS, mis == (uint64_t)threads * THREAD_ITERS ? "OK" : "FAIL");
    failures += g_counter64 != (uint64_t)threads * THREAD_ITERS;
    failures += split != (uint32_t)threads * THREAD_ITERS;
    failures += mis != (uint64_t)threads * THREAD_ITERS;

    g_pair[0] = g_pair[1] = 0;
    run_threads(th_cx16, threads, NULL);
    fprintf(rep, "lock cmpxchg16b 128-bit counter: lo=%llu hi=%llu (want %u, %u) %s\n",
            (unsigned long long)g_pair[0], (unsigned long long)g_pair[1], threads * THREAD_ITERS / 4 * 2,
            threads * THREAD_ITERS / 4,
            g_pair[0] == (uint64_t)threads * THREAD_ITERS / 4 * 2 && g_pair[1] == (uint64_t)threads * THREAD_ITERS / 4 ? "OK" : "FAIL");
    failures += !(g_pair[0] == (uint64_t)threads * THREAD_ITERS / 4 * 2 && g_pair[1] == (uint64_t)threads * THREAD_ITERS / 4);

    for (round = 0; round < 2; round++)
    {
        int rep_i;
        LONG total = 0;

        for (rep_i = 0; rep_i < 200; rep_i++)
        {
            HANDLE h[2];

            memset(g_mp, 0, sizeof(g_mp));
            g_mp_fail = 0;
            g_go = 0;
            h[0] = CreateThread(NULL, 0, th_mp_reader, NULL, 0, NULL);
            h[1] = CreateThread(NULL, 0, th_mp_writer, round ? (void *)1 : NULL, 0, NULL);
            Sleep(1);
            g_go = 1;
            WaitForMultipleObjects(2, h, TRUE, INFINITE);
            CloseHandle(h[0]); CloseHandle(h[1]);
            total += g_mp_fail;
        }
        fprintf(rep, "memory ordering, message passing (%s): %ld violations in 204800 %s\n",
                round ? "movnti + sfence" : "plain stores", total, total ? "FAIL" : "OK");
        failures += total != 0;
    }
    return failures;
}

/* -------------------------------------------------------------------- main */

static void hex(FILE *f, const uint8_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        fprintf(f, "%02x", p[i]);
}

int main(int argc, char **argv)
{
    int generate = argc > 1 && !strcmp(argv[1], "generate");
    const char *golden_path = argc > 2 ? argv[2] : "golden.txt";
    const char *report_path = argc > 3 ? argv[3] : "rosetta-report.txt";
    static uint8_t expect[NTESTS][NENVS][CAT_COUNT][64][32];
    static uint64_t golden[NTESTS][NENVS][CAT_COUNT];
    static int have[NTESTS];
    int t, e, c, mismatches = 0, failures;
    FILE *g, *rep;
    char brand[49] = {0};
    int regs[4];

    __cpuid(regs, 0x80000002); memcpy(brand, regs, 16);
    __cpuid(regs, 0x80000003); memcpy(brand + 16, regs, 16);
    __cpuid(regs, 0x80000004); memcpy(brand + 32, regs, 16);

    if (generate)
    {
        if (!(g = fopen(golden_path, "w")))
            return 2;
        fprintf(g, "# rosetta_test golden, cpu: %s\n", brand);
        for (t = 0; t < (int)NTESTS; t++)
            for (e = 0; e < NENVS; e++)
                for (c = 0; c < CAT_COUNT; c++)
                {
                    uint8_t in[96], out[32];
                    int dummy = -1, i;
                    uint64_t h = run_one(&tests[t], e, c, in, out, &dummy, NULL);
                    uint8_t outs[64][32];

                    /* record the first 64 outputs for precise diagnostics */
                    {
                        __attribute__((aligned(32))) uint8_t a[64], b[64], cc[64], o[64];
                        unsigned int saved = _mm_getcsr();
                        rng_state = 0x1234567ull * (uint64_t)(t + 1) + (uint64_t)c * 0x9999 + (uint64_t)e;
                        for (i = 0; i < 64; i++)
                        {
                            memset(a, 0, 64); memset(b, 0, 64); memset(cc, 0, 64);
                            fill(a, tests[t].kind, c); fill(b, tests[t].kind, c);
                            fill(cc, tests[t].kind == 1 ? 1 : tests[t].kind, c);
                            if (tests[t].kind == 1) fill(a + 32, 1, c);
                            memset(o, 0, 64);
                            _mm_setcsr(envs[e].mxcsr); tests[t].fn(a, b, cc, o); _mm_setcsr(saved);
                            memcpy(outs[i], o, 32);
                        }
                    }
                    fprintf(g, "%s %d %d %016llx ", tests[t].name, e, c, (unsigned long long)h);
                    for (i = 0; i < 64; i++)
                        hex(g, outs[i], 32);
                    fputc('\n', g);
                }
        fclose(g);
        printf("wrote %s (%u tests x %d environments x %d input kinds) on %s\n", golden_path,
                (unsigned)NTESTS, NENVS, CAT_COUNT, brand);
        return 0;
    }

    if (!(g = fopen(golden_path, "r")))
    {
        printf("cannot open %s\n", golden_path);
        return 2;
    }
    {
        static char line[8192];
        while (fgets(line, sizeof(line), g))
        {
            char name[64];
            unsigned long long h;
            int ee, cc, n = 0, i;
            char *p;

            if (line[0] == '#' || sscanf(line, "%63s %d %d %llx %n", name, &ee, &cc, &h, &n) < 4)
                continue;
            for (t = 0; t < (int)NTESTS && strcmp(tests[t].name, name); t++) {}
            if (t == (int)NTESTS || ee >= NENVS || cc >= CAT_COUNT)
                continue;
            golden[t][ee][cc] = h;
            have[t] = 1;
            p = line + n;
            for (i = 0; i < 64 * 32 && p[0] && p[1]; i++, p += 2)
            {
                unsigned int byte;
                sscanf(p, "%2x", &byte);
                expect[t][ee][cc][i / 32][i % 32] = (uint8_t)byte;
            }
        }
        fclose(g);
    }

    if (!(rep = fopen(report_path, "w")))
        rep = stdout;
    fprintf(rep, "Rosetta conformance report\ncpu reported to x86 code: %s\n\n", brand);
    for (t = 0; t < (int)NTESTS; t++)
    {
        if (!have[t])
            continue;
        for (e = 0; e < NENVS; e++)
            for (c = 0; c < CAT_COUNT; c++)
            {
                uint8_t in[96], out[32];
                int diff = -1;
                uint64_t h = run_one(&tests[t], e, c, in, out, &diff, (const uint8_t (*)[32])expect[t][e][c]);

                if (h == golden[t][e][c])
                    continue;
                mismatches++;
                fprintf(rep, "MISMATCH %-20s insn=%-12s env=%-9s inputs=%-7s", tests[t].name, tests[t].insn,
                        envs[e].name, cat_names[c]);
                if (diff >= 0)
                {
                    /* re-run to show the exact failing case */
                    __attribute__((aligned(32))) uint8_t a[64], b[64], cc2[64], o[64];
                    unsigned int saved = _mm_getcsr();
                    int i;
                    rng_state = 0x1234567ull * (uint64_t)(t + 1) + (uint64_t)c * 0x9999 + (uint64_t)e;
                    for (i = 0; i <= diff; i++)
                    {
                        memset(a, 0, 64); memset(b, 0, 64); memset(cc2, 0, 64);
                        fill(a, tests[t].kind, c); fill(b, tests[t].kind, c);
                        fill(cc2, tests[t].kind == 1 ? 1 : tests[t].kind, c);
                        if (tests[t].kind == 1) fill(a + 32, 1, c);
                    }
                    memset(o, 0, 64);
                    _mm_setcsr(envs[e].mxcsr); tests[t].fn(a, b, cc2, o); _mm_setcsr(saved);
                    fprintf(rep, " case #%d\n    a=", diff); hex(rep, a, 32);
                    fprintf(rep, "\n    b="); hex(rep, b, 32);
                    fprintf(rep, "\n    c="); hex(rep, cc2, 32);
                    fprintf(rep, "\n    got   ="); hex(rep, o, 32);
                    fprintf(rep, "\n    expect="); hex(rep, expect[t][e][c][diff], 32);
                    fputc('\n', rep);
                }
                else
                    fprintf(rep, " (differs after the first 64 cases)\n");
            }
    }
    fprintf(rep, "\n%d instruction/environment/input combinations differ from real x86.\n"
            "(rcp/rsqrt estimate mismatches are expected: Intel and AMD differ too. See the spec check below.)\n\n"
            "Estimate instructions:\n", mismatches);
    failures = estimate_tests(rep);
    fprintf(rep, "\nThreading:\n");
    failures += thread_tests(rep);
    fprintf(rep, "\n%d spec/threading checks failed.\n", failures);
    if (rep != stdout)
        fclose(rep);
    printf("%d instruction mismatches, %d spec/threading failures. Report: %s\n", mismatches, failures, report_path);
    return 0;
}
