#include <string.h>
#include "skin_inputs.h"

static uint32_t rng_state;

static uint32_t rnd(void)
{
    /* xorshift32 */
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static uint32_t rnd_below(uint32_t n)
{
    return rnd() % n;
}

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float rnd_float(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd() >> 8) / 16777216.0f;
}

void skin_make_inputs(struct skin_inputs *in, uint32_t seed)
{
    uint32_t i, g;

    rng_state = seed ? seed : 0x5eed1234u;
    memset(in, 0, sizeof(*in));

    /* t97: bind-pose vertices at words [0, SKIN_OUT_VERTS*4): x|y int16, z int16 | sign
     * bits in the high half, 3x10-bit packed normal/tangent + 2 flag bits, extra word.
     * Bone remap table (16-bit entries) from word 400000 on. */
    for (i = 0; i < SKIN_OUT_VERTS; i++)
    {
        in->raw[i * 4 + 0] = (rnd() & 0x3fff3fffu) ^ 0xe000e000u * (rnd() & 1);
        in->raw[i * 4 + 1] = (rnd() & 0xffff3fffu);
        in->raw[i * 4 + 2] = rnd();
        in->raw[i * 4 + 3] = rnd();
    }
    for (i = 400000; i < SKIN_RAW_WORDS; i++)
        in->raw[i] = rnd_below(2048) | (rnd_below(2048) << 16);

    /* t101: per-instance records (16 words). [7] bit 4 enables the position offset,
     * [8..10] and the record named by entry[6]>>12 hold 16-bit offsets, [15] the
     * position scale (float). */
    for (i = 0; i < SKIN_INST_RECORDS; i++)
    {
        uint32_t *r = in->inst + i * 16;
        uint32_t k;

        for (k = 0; k < 16; k++)
            r[k] = rnd_below(0x800);
        r[7] = rnd() & 0x10;
        r[8] = rnd() & 0xffff;
        r[9] = rnd() & 0xffff;
        r[10] = rnd() & 0xffff;
        r[15] = f2u(1.0f / (float)(256u << rnd_below(4)));
    }

    /* t0: bone matrices, 4 float4 columns (x axis, y axis, z axis, translation). */
    for (i = 0; i < SKIN_BONES; i++)
    {
        uint32_t *m = in->bones + i * 16;
        uint32_t c, r;

        for (c = 0; c < 4; c++)
        {
            for (r = 0; r < 3; r++)
                m[c * 4 + r] = f2u(c < 3 ? rnd_float(-0.7f, 0.7f) : rnd_float(-6.0f, 6.0f));
            m[c * 4 + 3] = 0;
        }
    }

    /* t1: random packed influence/weight words everywhere, then real block headers. */
    for (i = 0; i < SKIN_DATA_WORDS; i++)
        in->data[i] = rnd();

    /* t2: work entries, and the headers they point at. */
    for (g = 0; g < SKIN_GROUPS; g++)
    {
        uint32_t *e = in->entries + g * 8;
        uint32_t count = 32 + rnd_below(SKIN_MAX_VERTS - 31);
        uint32_t blocks = (count + 31) / 32, b;
        uint32_t hdr = 600000 + g * 64, wbase = g * 8192;
        uint32_t mode = (g % 8 == 7) ? 1 + rnd_below(3) : 0;   /* mostly the main path */
        uint32_t record = rnd_below(SKIN_INST_RECORDS);

        e[0] = g * SKIN_MAX_VERTS;              /* first input vertex (t97) */
        e[1] = g * SKIN_MAX_VERTS;              /* first output vertex (u0) */
        e[2] = hdr | (mode << 30);              /* first block header (t1) | mode */
        e[3] = wbase;                           /* influence data base (t1) */
        e[4] = rnd_below(1024);                 /* bone palette base (t0) */
        e[5] = (g & 1) ? 0xffffffffu : 800000 + rnd_below(1000);  /* remap table or none */
        e[6] = count | (rnd_below(SKIN_INST_RECORDS) << 12);
        e[7] = record;
        for (b = 0; b < blocks; b++)
        {
            /* influences-1 in bits 21-24 (mostly 1-8 influences), bone base bits 8-11
             * in 25-28, bit 20 = alternate weight format, low 20 bits = data offset */
            uint32_t infl = rnd_below(4) ? rnd_below(8) : rnd_below(16);
            in->data[hdr + b] = (b * 128) | ((rnd() & 1) << 20) | (infl << 21) | (rnd_below(4) << 25);
        }
    }
}
