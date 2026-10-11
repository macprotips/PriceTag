#!/usr/bin/env bash
# Rebuilds sm2fix-skinning.dxil from the game's CS_ModelSkinning (hash 3d50acdb571dce22,
# e.g. sm2fix-dumps/3d50acdb571dce22_cs.dxil from a dump_compute run). Needs a DXC build
# with dxc, dxa and dxv (github.com/microsoft/DirectXShaderCompiler: ninja dxc dxa dxv).
#   ./make-fix.sh path/to/3d50acdb571dce22_cs.dxil path/to/dxc/build/bin
set -euo pipefail
cd "$(dirname "$0")"
IN=$1 BIN=$2
mkdir -p build
"$BIN/dxc" -dumpbin "$IN" -Fc build/skin.ll
python3 unalloca.py build/skin.ll build/skin-fixed.ll        # arrays -> selects
python3 name_values.py build/skin-fixed.ll build/skin-named.ll
"$BIN/dxa" -o sm2fix-skinning.dxil build/skin-named.ll       # assemble + sign
"$BIN/dxv" sm2fix-skinning.dxil                              # validate
