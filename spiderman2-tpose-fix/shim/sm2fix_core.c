#include "sm2fix_core.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

uint64_t sm2_shader_hash(const void *code, size_t size)
{
    /* Matches vkd3d-proton's hash_fnv1_init / hash_fnv1_iterate_u8 exactly
     * (multiply, then xor), so hashes from its quirk tables can be reused as-is. */
    const uint8_t *p = code;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < size; i++)
        h = (h * 0x100000001b3ull) ^ p[i];
    return h;
}

/* D3D12_PIPELINE_STATE_SUBOBJECT_TYPE values we care about. */
enum
{
    SUB_ROOT_SIGNATURE = 0,
    SUB_CS = 6,
    SUB_NODE_MASK = 18,
    SUB_CACHED_PSO = 19,
    SUB_FLAGS = 20,
};

static int is_graphics_subobject(uint32_t type)
{
    /* VS..GS, stream output .. sample desc, depth-stencil1, view instancing,
     * AS/MS, depth-stencil2, rasterizer1/2. Any of these means "not compute". */
    return (type >= 1 && type <= 5) || (type >= 7 && type <= 17) || type == 21 || type == 22
            || (type >= 24 && type <= 28);
}

/* Each stream subobject is { TYPE (4 bytes); inner; } aligned to sizeof(void *),
 * as laid out by d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT on 64-bit. */
static size_t subobject_size(uint32_t type)
{
    switch (type)
    {
        case SUB_ROOT_SIGNATURE: return 16; /* 4 + pad + pointer */
        case SUB_CS:             return 24; /* 4 + pad + D3D12_SHADER_BYTECODE (16) */
        case SUB_NODE_MASK:      return 8;
        case SUB_CACHED_PSO:     return 24; /* 4 + pad + D3D12_CACHED_PIPELINE_STATE (16) */
        case SUB_FLAGS:          return 8;
        default:                 return 0;
    }
}

enum sm2_stream_result sm2_stream_find_cs(const void *stream, size_t size,
        const void **cs_code, size_t *cs_size)
{
    const uint8_t *base = stream;
    size_t offset = 0;
    int found = 0;

    if (!stream)
        return SM2_STREAM_MALFORMED;

    while (offset < size)
    {
        uint32_t type;
        size_t sub_size;

        if (size - offset < sizeof(uint32_t))
            return SM2_STREAM_MALFORMED;
        memcpy(&type, base + offset, sizeof(type));

        if (is_graphics_subobject(type))
            return SM2_STREAM_GRAPHICS;
        if (!(sub_size = subobject_size(type)))
            return found ? SM2_STREAM_COMPUTE : SM2_STREAM_UNKNOWN;
        if (size - offset < sub_size)
            return SM2_STREAM_MALFORMED;

        if (type == SUB_CS)
        {
            const void *code;
            uint64_t length;

            memcpy(&code, base + offset + 8, sizeof(code));
            memcpy(&length, base + offset + 16, sizeof(length));
            *cs_code = code;
            *cs_size = (size_t)length;
            found = 1;
        }
        offset += sub_size;
    }

    return found ? SM2_STREAM_COMPUTE : SM2_STREAM_NO_CS;
}

void sm2_config_defaults(struct sm2_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = SM2_MODE_HASH;
    cfg->log_shaders = 0;
    cfg->hash_count = 1;
    cfg->hashes[0] = SM2_DEFAULT_HASH;
}

static int key_equals(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    }
    return !*a && !*b;
}

static void parse_hash_list(struct sm2_config *cfg, const char *value)
{
    const char *p = value;

    cfg->hash_count = 0;
    while (*p && cfg->hash_count < SM2_MAX_HASHES)
    {
        char *end;
        uint64_t h;

        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        if (!*p)
            break;
        h = strtoull(p, &end, 16);
        if (end == p)
            break;
        cfg->hashes[cfg->hash_count++] = h;
        p = end;
    }
}

int sm2_config_set(struct sm2_config *cfg, const char *key, const char *value)
{
    if (key_equals(key, "mode"))
    {
        if (key_equals(value, "off"))
            cfg->mode = SM2_MODE_OFF;
        else if (key_equals(value, "all"))
            cfg->mode = SM2_MODE_ALL;
        else
            cfg->mode = SM2_MODE_HASH;
        return 0;
    }
    if (key_equals(key, "log_shaders"))
    {
        cfg->log_shaders = atoi(value) != 0;
        return 0;
    }
    if (key_equals(key, "hashes"))
    {
        parse_hash_list(cfg, value);
        return 0;
    }
    if (key_equals(key, "real_d3d12"))
    {
        strncpy(cfg->real_d3d12, value, sizeof(cfg->real_d3d12) - 1);
        cfg->real_d3d12[sizeof(cfg->real_d3d12) - 1] = 0;
        return 0;
    }
    return -1;
}

static void trim(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;

    while (len && isspace((unsigned char)s[len - 1]))
        s[--len] = 0;
    while (s[start] && isspace((unsigned char)s[start]))
        start++;
    if (start)
        memmove(s, s + start, len - start + 1);
}

void sm2_config_parse_text(struct sm2_config *cfg, const char *text)
{
    const char *line = text;

    while (line && *line)
    {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        char buf[1024], *eq;

        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, line, len);
        buf[len] = 0;
        trim(buf);

        if (buf[0] && buf[0] != '#' && buf[0] != ';' && buf[0] != '[' && (eq = strchr(buf, '=')))
        {
            *eq = 0;
            trim(buf);
            trim(eq + 1);
            sm2_config_set(cfg, buf, eq + 1);
        }
        line = next ? next + 1 : NULL;
    }
}

int sm2_config_has_hash(const struct sm2_config *cfg, uint64_t hash)
{
    unsigned int i;

    for (i = 0; i < cfg->hash_count; i++)
    {
        if (cfg->hashes[i] == hash)
            return 1;
    }
    return 0;
}
