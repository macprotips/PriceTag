/* Platform-independent pieces of the Spider-Man 2 barrier shim: shader hashing,
 * pipeline-stream parsing and config parsing. Kept free of Windows headers so the
 * logic can be unit-tested natively (see ../tests/test_core.c). */
#ifndef SM2FIX_CORE_H
#define SM2FIX_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Same hash vkd3d-proton uses to identify shaders (vkd3d_shader_hash): FNV-1 style
 * over the raw DXBC/DXIL container bytes handed to CreateComputePipelineState. */
uint64_t sm2_shader_hash(const void *code, size_t size);

/* The compute shader vkd3d-proton forces a post-dispatch barrier on for
 * Spider-Man2.exe (vkd3d-proton commit cc9d6cc7, "Add missing barrier in Spider Man 2"). */
#define SM2_DEFAULT_HASH 0x324071d329f05cccull

enum sm2_stream_result
{
    SM2_STREAM_COMPUTE = 0,   /* compute pipeline, CS found */
    SM2_STREAM_GRAPHICS,      /* has a graphics/mesh stage: not interesting */
    SM2_STREAM_NO_CS,         /* parsed fine, no CS present */
    SM2_STREAM_UNKNOWN,       /* hit a subobject type we can't size: gave up */
    SM2_STREAM_MALFORMED,
};

/* Walks a D3D12_PIPELINE_STATE_STREAM_DESC payload (64-bit layout) looking for the CS. */
enum sm2_stream_result sm2_stream_find_cs(const void *stream, size_t size,
        const void **cs_code, size_t *cs_size);

enum sm2_mode
{
    SM2_MODE_OFF = 0,   /* pass-through, only logs */
    SM2_MODE_HASH,      /* barrier after dispatches of the listed shaders (default) */
    SM2_MODE_ALL,       /* barrier after every compute dispatch (diagnostic) */
};

#define SM2_MAX_HASHES 32

struct sm2_config
{
    enum sm2_mode mode;
    int log_shaders;            /* log every compute shader hash seen */
    unsigned int hash_count;
    uint64_t hashes[SM2_MAX_HASHES];
    char real_d3d12[512];       /* optional explicit path of the real d3d12.dll */
};

void sm2_config_defaults(struct sm2_config *cfg);
/* Applies one "key=value" setting. Returns 0 if the key was recognised. */
int sm2_config_set(struct sm2_config *cfg, const char *key, const char *value);
/* Applies every line of an ini-style text buffer ('#'/';' comments, [sections] ignored). */
void sm2_config_parse_text(struct sm2_config *cfg, const char *text);
int sm2_config_has_hash(const struct sm2_config *cfg, uint64_t hash);

#endif
