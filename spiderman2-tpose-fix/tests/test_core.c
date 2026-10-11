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
    CHECK(sm2_config_set(&cfg, "mode", "hash") == 0 && cfg.mode == SM2_MODE_HASH);
    CHECK(sm2_config_set(&cfg, "nope", "1") == -1);
}

int main(void)
{
    test_hash();
    test_stream_compute();
    test_stream_graphics_and_errors();
    test_config();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
