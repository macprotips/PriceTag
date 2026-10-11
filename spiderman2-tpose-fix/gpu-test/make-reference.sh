#!/usr/bin/env bash
# Builds the reference output for skin_gpu_test.exe (Linux: needs dxil-spirv, spirv-as/dis,
# libvulkan-dev and mesa-vulkan-drivers). Input: the game's CS_ModelSkinning DXIL, e.g.
# from sm2fix-dumps/3d50acdb571dce22_cs.dxil.
#   ./make-reference.sh path/to/3d50acdb571dce22_cs.dxil path/to/dxil-spirv
set -euo pipefail
cd "$(dirname "$0")"
DXIL=$1 DXIL_SPIRV=${2:-dxil-spirv}
OUT=build && mkdir -p "$OUT"
"$DXIL_SPIRV" "$DXIL" --typed-uav-read-without-format --output "$OUT/skin-orig.spv"
spirv-dis "$OUT/skin-orig.spv" -o "$OUT/skin.spvasm"
# dxil-spirv keeps D3D registers as bindings (set = space); flatten them into set 0.
python3 - "$OUT/skin.spvasm" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
names = {}
for var, kind in re.findall(r'(%\d+) = OpVariable %_ptr_UniformConstant_(\d+) UniformConstant', s):
    names[var] = kind
dec = dict(re.findall(r'OpDecorate (%\d+) Binding (\d+)\n', s))
sets = dict(re.findall(r'OpDecorate (%\d+) DescriptorSet (\d+)\n', s))
remap = {('0', '0', False): 0, ('0', '1', False): 1, ('0', '2', False): 2, ('1', '97', False): 3, ('1', '101', False): 4, ('0', '0', True): 5}
storage = set(re.findall(r'(%\d+) = OpVariable %_ptr_UniformConstant_(\d+) UniformConstant', s))
img2 = set(re.findall(r'(%\d+) = OpTypeImage %uint Buffer 0 0 0 2 ', s))
for var in dec:
    is_uav = ('%' + names[var]) in img2
    b = remap[(sets[var], dec[var], is_uav)]
    s = s.replace('OpDecorate %s DescriptorSet %s\n' % (var, sets[var]), 'OpDecorate %s DescriptorSet 0\n' % var)
    s = s.replace('OpDecorate %s Binding %s\n' % (var, dec[var]), 'OpDecorate %s Binding %d\n' % (var, b))
open(p, 'w').write(s)
PY
spirv-as "$OUT/skin.spvasm" -o "$OUT/skin.spv" --target-env vulkan1.1
spirv-val "$OUT/skin.spv"
cc -O2 -o "$OUT/skin_ref_vk" skin_ref_vk.c skin_inputs.c -lvulkan
"$OUT/skin_ref_vk" "$OUT/skin.spv" golden-skin.bin
