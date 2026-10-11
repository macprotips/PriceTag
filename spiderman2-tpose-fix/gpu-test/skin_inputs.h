/* Deterministic synthetic inputs for the game's CS_ModelSkinning compute shader.
 * The same generator feeds the Vulkan reference run (skin_ref_vk.c, on Linux) and the
 * D3D12 run under D3DMetal (skin_gpu_test.c, on the Mac), so their outputs can be
 * compared word for word. Every index the shader computes stays inside its buffer:
 * out-of-bounds reads behave differently across GPUs and would hide the real result.
 *
 * Shader bindings (from its PSV0 part):
 *   t97 space1  ByteAddressBuffer       bind-pose vertices (16 B each) + bone remap table
 *   t101 space1 StructuredBuffer (64 B) per-instance records
 *   t0          StructuredBuffer (64 B) bone matrices (3x4 floats in 4 float4 columns)
 *   t1          Buffer<uint>            block headers + packed influences/weights
 *   t2          StructuredBuffer (32 B) SkinningSB work entries, one per thread group
 *   u0          RWBuffer<uint4>         skinned output vertices (16 B each) */
#ifndef SKIN_INPUTS_H
#define SKIN_INPUTS_H

#include <stdint.h>

#define SKIN_GROUPS        64
#define SKIN_MAX_VERTS     1280                 /* per group (0x500, as the game uses) */
#define SKIN_OUT_VERTS     (SKIN_GROUPS * SKIN_MAX_VERTS)
#define SKIN_RAW_WORDS     (1u << 20)           /* t97 */
#define SKIN_INST_RECORDS  16                   /* t101 */
#define SKIN_BONES         16384                /* t0 */
#define SKIN_DATA_WORDS    (1u << 20)           /* t1 */

struct skin_inputs
{
    uint32_t raw[SKIN_RAW_WORDS];
    uint32_t inst[SKIN_INST_RECORDS * 16];
    uint32_t bones[SKIN_BONES * 16];
    uint32_t data[SKIN_DATA_WORDS];
    uint32_t entries[SKIN_GROUPS * 8];
};

/* Fills *in for the given seed. */
void skin_make_inputs(struct skin_inputs *in, uint32_t seed);

#endif
