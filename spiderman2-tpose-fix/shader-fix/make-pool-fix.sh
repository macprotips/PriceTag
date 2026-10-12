#!/usr/bin/env bash
# Rebuilds the pool_fix shaders (sm2fix-pool-<hash>.dxil): the game's CS_ModelSkinning
# (3d50acdb571dce22, pool at t1) and vertex-morph shader (14902794dd7134e4, pool at t0),
# with their typed Buffer<uint> pool reads turned into raw reads (typed2raw.py).
# Input: a folder with <hash>_cs.dxil dumps (dump_compute). Needs dxc, dxa, dxv.
#   ./make-pool-fix.sh path/to/sm2fix-dumps path/to/dxc/build/bin
set -euo pipefail
cd "$(dirname "$0")"
IN=$1 BIN=$2
mkdir -p build
for spec in 3d50acdb571dce22:0:1 14902794dd7134e4:0:0; do
    IFS=: read -r hash space reg <<< "$spec"
    "$BIN/dxc" -dumpbin "$IN/${hash}_cs.dxil" -Fc "build/$hash.ll"
    python3 typed2raw.py "build/$hash.ll" "build/$hash-raw.ll" "$space" "$reg"
    python3 name_values.py "build/$hash-raw.ll" "build/$hash-named.ll"
    "$BIN/dxa" -o "build/$hash-unsigned.dxil" "build/$hash-named.ll"
    "$BIN/dxv" "build/$hash-unsigned.dxil" -o "sm2fix-pool-$hash.dxil"   # validate + sign
done
