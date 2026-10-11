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
/* The game's object-space skinning compute shader, CS_ModelSkinning. */
#define SM2_DEFAULT_SKINNING_HASH 0x3d50acdb571dce22ull

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

/* Byte size of one stream subobject of `type` (header + padding + payload), 0 if unknown. */
size_t sm2_stream_subobject_size(uint32_t type);
/* Calls fn for every shader subobject (VS, PS, DS, HS, GS, CS, AS, MS). Returns the number
 * of subobjects walked, or -1 if the stream has a type we can't size. */
int sm2_stream_for_each_shader(const void *stream, size_t size,
        void (*fn)(void *ctx, uint32_t type, const void *code, size_t len), void *ctx);

enum sm2_mode
{
    SM2_MODE_OFF = 0,   /* pass-through, only logs */
    SM2_MODE_HASH,      /* barrier after dispatches of the listed shaders (default) */
    SM2_MODE_ALL,       /* barrier after every compute dispatch (diagnostic) */
    SM2_MODE_MAX,       /* ALL + wait for the GPU after every queue submission (diagnostic, slow) */
};

#define SM2_MAX_HASHES 32

struct sm2_config
{
    enum sm2_mode mode;
    int log_shaders;            /* log every compute shader hash seen */
    unsigned int hash_count;
    uint64_t hashes[SM2_MAX_HASHES];
    char real_d3d12[512];       /* optional explicit path of the real d3d12.dll */
    unsigned int spoof;         /* SM2_SPOOF_* bits: features to hide from the game */
    int dump_compute;           /* dump every compute shader to sm2fix-dumps */
    int dump_graphics;          /* dump every vertex/mesh shader to sm2fix-dumps */
    int minmax;                 /* enum sm2_minmax_rewrite: what to do with min/max samplers */
    int profile;                /* count dispatches per compute shader and log the top ones */
    unsigned int cpu_count;     /* 0 = untouched, else max processors the game is told about */
    int trace_queries;          /* log GPU queries (timestamps, occlusion) and the values read back */
    int probe_skin;             /* hook the game's skin-job launcher and log its gate values */
    uint32_t probe_skin_rva;    /* launcher RVA (default for the analysed build) */
    uint32_t probe_jobsys_rva;  /* job-system state RVA (flag at +0x24) */
    uint32_t probe_sb_rva;      /* stub that creates the SkinningSB buffer (sized by ModelSkinGroupMax) */
    uint32_t probe_dispatch_rva;/* function that fills SkinningSB and dispatches CS_ModelSkinning */
    uint64_t skinning_hash;     /* CS_ModelSkinning's hash (overridable for tests) */
    unsigned int snapshot_ms;   /* probe_skin: period of the binding/palette snapshots */
    int replace_skinning;       /* swap CS_ModelSkinning for sm2fix-skinning.dxil (array-free rewrite) */
    int skip_skinning;          /* diagnostic: drop every Dispatch while CS_ModelSkinning is bound */
    unsigned int skin_budget;   /* 0 = untouched, else raise RenderBudgets.ModelSkinGroupMax to at least this */
    int fake_timestamps;        /* replace D3DMetal's GPU timestamps (60 Hz, all zero on M1-M4) with real CPU time */
    unsigned int pso_delay_ms;  /* sleep this long in every pipeline creation (imitates a cold shader cache) */
};

/* Feature answers sm2fix can change, to steer the game onto a different code path. */
#define SM2_SPOOF_SM65      0x01u  /* report Shader Model 6.5 instead of 6.6 */
#define SM2_SPOOF_NO_INT64  0x02u  /* OPTIONS1.Int64ShaderOps = 0 */
#define SM2_SPOOF_NO_FP16   0x04u  /* OPTIONS4.Native16BitShaderOpsSupported = 0 */
#define SM2_SPOOF_UMA       0x08u  /* ARCHITECTURE(1).UMA = CacheCoherentUMA = 1 */
#define SM2_SPOOF_NO_RT     0x10u  /* OPTIONS5.RaytracingTier = not supported */

void sm2_config_defaults(struct sm2_config *cfg);
/* Applies one "key=value" setting. Returns 0 if the key was recognised. */
int sm2_config_set(struct sm2_config *cfg, const char *key, const char *value);
/* Applies every line of an ini-style text buffer ('#'/';' comments, [sections] ignored). */
void sm2_config_parse_text(struct sm2_config *cfg, const char *text);
int sm2_config_has_hash(const struct sm2_config *cfg, uint64_t hash);

#endif
