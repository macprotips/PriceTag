/* Native unit tests for shim/sm2fix_core.c. Run: ./tests/run.sh */
#include "../shim/sm2fix_core.h"

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
}

int main(void)
{
    test_hash();
    test_stream_compute();
    test_stream_graphics_and_errors();
    test_stream_walk_graphics();
    test_config();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
