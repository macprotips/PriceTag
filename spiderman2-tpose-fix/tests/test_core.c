/* Native unit tests for shim/sm2fix_core.c. Run: ./tests/run.sh */
#include "../shim/sm2fix_core.h"
#include "../shim/sm2fix_dxbc.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Reference copy of vkd3d-proton's hash (include/private/hashmap.h + vkd3d_shader_hash). */
static uint64_t vkd3d_reference_hash(const uint8_t *code, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h * 0x100000001b3ull) ^ code[i];
    return h;
}

static void test_hash(void)
{
    uint8_t buf[300];
    size_t i;

    for (i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 7 + 3);
    CHECK(sm2_shader_hash(buf, sizeof(buf)) == vkd3d_reference_hash(buf, sizeof(buf)));
    CHECK(sm2_shader_hash(buf, 0) == 0xcbf29ce484222325ull);
    /* one byte: (offset * prime) ^ byte */
    CHECK(sm2_shader_hash("A", 1) == ((0xcbf29ce484222325ull * 0x100000001b3ull) ^ 'A'));
}

/* Builds streams the way d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT lays them out. */
struct sub_ptr { uint32_t type; uint32_t pad; void *p; };
struct sub_bytecode { uint32_t type; uint32_t pad; const void *code; uint64_t size; };
struct sub_u32 { uint32_t type; uint32_t value; };

static void test_stream_compute(void)
{
    static const char shader[] = "DXBC-fake-shader";
    struct
    {
        struct sub_ptr root;
        struct sub_bytecode cs;
        struct sub_u32 node_mask;
        struct sub_bytecode cached;
        struct sub_u32 flags;
    } stream = {
        {0, 0, (void *)0x1234},
        {6, 0, shader, sizeof(shader)},
        {18, 1},
        {19, 0, NULL, 0},
        {20, 0},
    };
    const void *code = NULL;
    size_t size = 0;

    CHECK(sizeof(stream) == 16 + 24 + 8 + 24 + 8);
    CHECK(sm2_stream_find_cs(&stream, sizeof(stream), &code, &size) == SM2_STREAM_COMPUTE);
    CHECK(code == shader);
    CHECK(size == sizeof(shader));
}

static void test_stream_graphics_and_errors(void)
{
    struct { struct sub_ptr root; struct sub_bytecode vs; } gfx = {{0, 0, NULL}, {1, 0, "x", 1}};
    struct { struct sub_ptr root; struct sub_u32 weird; } unknown = {{0, 0, NULL}, {99, 0}};
    struct sub_ptr root_only = {0, 0, NULL};
    const void *code = NULL;
    size_t size = 0;

    CHECK(sm2_stream_find_cs(&gfx, sizeof(gfx), &code, &size) == SM2_STREAM_GRAPHICS);
    CHECK(sm2_stream_find_cs(&unknown, sizeof(unknown), &code, &size) == SM2_STREAM_UNKNOWN);
    CHECK(sm2_stream_find_cs(&root_only, sizeof(root_only), &code, &size) == SM2_STREAM_NO_CS);
    CHECK(sm2_stream_find_cs(&root_only, 12, &code, &size) == SM2_STREAM_MALFORMED);
    CHECK(sm2_stream_find_cs(NULL, 0, &code, &size) == SM2_STREAM_MALFORMED);
}

static int walked;
static const void *walked_code[4];
static uint32_t walked_type[4];

static void on_shader(void *ctx, uint32_t type, const void *code, size_t len)
{
    (void)ctx;
    (void)len;
    if (walked < 4)
    {
        walked_type[walked] = type;
        walked_code[walked] = code;
    }
    walked++;
}

static void test_stream_walk_graphics(void)
{
    /* root sig, VS, PS, blend (332 -> 336), rasterizer (48), RT formats (40), sample desc (16) */
    uint8_t stream[16 + 24 + 24 + 336 + 48 + 40 + 16];
    static const char vs[] = "vs", ps[] = "ps";
    struct sub_bytecode sb;
    uint32_t type;
    size_t off = 0;

    CHECK(sm2_stream_subobject_size(0) == 16);
    CHECK(sm2_stream_subobject_size(6) == 24);
    CHECK(sm2_stream_subobject_size(8) == 336);
    CHECK(sm2_stream_subobject_size(10) == 48);
    CHECK(sm2_stream_subobject_size(15) == 40);
    CHECK(sm2_stream_subobject_size(17) == 16);
    CHECK(sm2_stream_subobject_size(22) == 32);
    CHECK(sm2_stream_subobject_size(23) == 0);
    CHECK(sm2_stream_subobject_size(99) == 0);

    memset(stream, 0, sizeof(stream));
    type = 0; memcpy(stream + off, &type, 4); off += 16;
    sb.type = 1; sb.pad = 0; sb.code = vs; sb.size = sizeof(vs); memcpy(stream + off, &sb, 24); off += 24;
    sb.type = 2; sb.code = ps; sb.size = sizeof(ps); memcpy(stream + off, &sb, 24); off += 24;
    type = 8; memcpy(stream + off, &type, 4); off += 336;
    type = 10; memcpy(stream + off, &type, 4); off += 48;
    type = 15; memcpy(stream + off, &type, 4); off += 40;
    type = 17; memcpy(stream + off, &type, 4); off += 16;
    CHECK(off == sizeof(stream));

    walked = 0;
    CHECK(sm2_stream_for_each_shader(stream, sizeof(stream), on_shader, NULL) == 7);
    CHECK(walked == 2);
    CHECK(walked_type[0] == 1 && walked_code[0] == vs);
    CHECK(walked_type[1] == 2 && walked_code[1] == ps);

    type = 99; memcpy(stream + 16, &type, 4);
    CHECK(sm2_stream_for_each_shader(stream, sizeof(stream), on_shader, NULL) == -1);
}

/* Builds a DXBC root signature (RTS0 v1.1) with two static samplers. */
static size_t build_rootsig(uint8_t *out, uint32_t filter0, uint32_t filter1)
{
    uint32_t rts0[6 + 2 * 13], hdr[9], sum[4];
    size_t size = sizeof(hdr) + 8 + sizeof(rts0);
    uint32_t part_size = sizeof(rts0);
    unsigned int i;

    memset(rts0, 0, sizeof(rts0));
    rts0[0] = 2;            /* version 1.1 */
    rts0[3] = 2;            /* static samplers */
    rts0[4] = 24;           /* offset */
    rts0[6] = filter0;
    rts0[6 + 13] = filter1;
    for (i = 0; i < 2; i++)
        rts0[6 + i * 13 + 10] = i; /* shader register */
    memcpy(hdr, "DXBC", 4);
    memset(hdr + 1, 0, 16);
    hdr[5] = 1;
    hdr[6] = (uint32_t)size;
    hdr[7] = 1;             /* part count */
    hdr[8] = sizeof(hdr);   /* part offset */
    memcpy(out, hdr, sizeof(hdr));
    memcpy(out + sizeof(hdr), "RTS0", 4);
    memcpy(out + sizeof(hdr) + 4, &part_size, 4);
    memcpy(out + sizeof(hdr) + 8, rts0, sizeof(rts0));
    sm2_dxbc_checksum(out, size, sum);
    memcpy(out + 4, sum, 16);
    return size;
}

static void test_rootsig_samplers(void)
{
    uint8_t blob[512];
    unsigned int minmax, total;
    uint32_t first = 0, f0, f1;
    size_t size = build_rootsig(blob, 0x195 /* MAXIMUM_MIN_MAG_MIP_LINEAR */, 0x15 /* MIN_MAG_MIP_LINEAR */);

    CHECK(sm2_dxbc_checksum_ok(blob, size));
    CHECK(sm2_filter_is_minmax(0x195) && sm2_filter_is_minmax(0x100) && !sm2_filter_is_minmax(0x95));
    CHECK(sm2_filter_rewrite(0x195, SM2_MINMAX_LINEAR) == 0x15);
    CHECK(sm2_filter_rewrite(0x195, SM2_MINMAX_POINT) == 0);
    CHECK(sm2_filter_rewrite(0x15, SM2_MINMAX_POINT) == 0x15);

    CHECK(sm2_rootsig_scan_samplers(blob, size, SM2_MINMAX_KEEP, &minmax, &total, &first) == 0);
    CHECK(minmax == 1 && total == 2 && first == 0x195);

    CHECK(sm2_rootsig_scan_samplers(blob, size, SM2_MINMAX_LINEAR, &minmax, &total, &first) == 0);
    memcpy(&f0, blob + 36 + 8 + 24, 4);
    memcpy(&f1, blob + 36 + 8 + 24 + 52, 4);
    CHECK(f0 == 0x15 && f1 == 0x15);
    CHECK(sm2_dxbc_checksum_ok(blob, size)); /* checksum was fixed after the edit */

    blob[0] = 'X';
    CHECK(sm2_rootsig_scan_samplers(blob, size, SM2_MINMAX_KEEP, &minmax, &total, &first) == -1);
}

static void test_config(void)
{
    struct sm2_config cfg;

    sm2_config_defaults(&cfg);
    CHECK(cfg.mode == SM2_MODE_HASH);
    CHECK(cfg.hash_count == 1 && cfg.hashes[0] == SM2_DEFAULT_HASH);
    CHECK(sm2_config_has_hash(&cfg, 0x324071d329f05cccull));

    sm2_config_parse_text(&cfg,
            "# comment\r\n"
            "[section]\n"
            "  Mode = ALL \r\n"
            "log_shaders=1\n"
            "hashes = 0x1111, 2222 ,abcdef0123456789\n"
            "real_d3d12 = C:\\\\x\\\\d3d12.dll\n"
            "unknown = 5\n");
    CHECK(cfg.mode == SM2_MODE_ALL);
    CHECK(cfg.log_shaders == 1);
    CHECK(cfg.hash_count == 3);
    CHECK(cfg.hashes[0] == 0x1111 && cfg.hashes[1] == 0x2222 && cfg.hashes[2] == 0xabcdef0123456789ull);
    CHECK(!sm2_config_has_hash(&cfg, SM2_DEFAULT_HASH));
    CHECK(strcmp(cfg.real_d3d12, "C:\\\\x\\\\d3d12.dll") == 0);

    CHECK(sm2_config_set(&cfg, "mode", "off") == 0 && cfg.mode == SM2_MODE_OFF);
    CHECK(sm2_config_set(&cfg, "mode", "MAX") == 0 && cfg.mode == SM2_MODE_MAX);
    CHECK(sm2_config_set(&cfg, "mode", "hash") == 0 && cfg.mode == SM2_MODE_HASH);
    CHECK(sm2_config_set(&cfg, "nope", "1") == -1);

    CHECK(sm2_config_set(&cfg, "spoof", "sm65, NO_FP16,bogus") == 0);
    CHECK(cfg.spoof == (SM2_SPOOF_SM65 | SM2_SPOOF_NO_FP16));
    CHECK(sm2_config_set(&cfg, "spoof", "uma no_rt no_int64") == 0);
    CHECK(cfg.spoof == (SM2_SPOOF_UMA | SM2_SPOOF_NO_RT | SM2_SPOOF_NO_INT64));
    CHECK(sm2_config_set(&cfg, "spoof", "") == 0 && cfg.spoof == 0);
    CHECK(sm2_config_set(&cfg, "dump_compute", "1") == 0 && cfg.dump_compute == 1);
    CHECK(sm2_config_set(&cfg, "minmax", "point") == 0 && cfg.minmax == SM2_MINMAX_POINT);
    CHECK(sm2_config_set(&cfg, "minmax", "linear") == 0 && cfg.minmax == SM2_MINMAX_LINEAR);
    CHECK(sm2_config_set(&cfg, "minmax", "keep") == 0 && cfg.minmax == SM2_MINMAX_KEEP);
    CHECK(sm2_config_set(&cfg, "pso_delay_ms", "5") == 0 && cfg.pso_delay_ms == 5);
    CHECK(sm2_config_set(&cfg, "pso_delay_ms", "99999") == 0 && cfg.pso_delay_ms == 0);
    CHECK(sm2_config_set(&cfg, "cpu_count", "8") == 0 && cfg.cpu_count == 8);
}

int main(void)
{
    test_hash();
    test_stream_compute();
    test_stream_graphics_and_errors();
    test_stream_walk_graphics();
    test_rootsig_samplers();
    test_config();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
