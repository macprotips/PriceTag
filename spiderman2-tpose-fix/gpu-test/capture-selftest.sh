#!/usr/bin/env bash
# Checks sm2fix's dispatch capture + replay.exe end to end on a real D3D12 driver (Linux,
# Wine, vkd3d-proton on lavapipe): capture_demo.exe runs the game's CS_ZivaTensorSkinning
# through the shim with capture on, then replay.exe re-runs the capture and must match.
#   capture-selftest.sh <Spider-Man2.exe> <dir with vkd3d-proton d3d12.dll + d3d12core.dll>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="$(realpath "$1")" VKD="$(realpath "$2")"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/cap" "$T/rep"
cp "$HERE/../build/d3d12.dll" "$T/cap/d3d12.dll"
cp "$VKD/d3d12.dll" "$T/cap/real_d3d12.dll"
cp "$VKD/d3d12core.dll" "$T/cap/"
cp "$VKD/d3d12.dll" "$VKD/d3d12core.dll" "$T/rep/"
cp "$HERE/capture_demo.exe" "$T/cap/"
cp "$HERE/replay.exe" "$T/rep/"
export WINEDLLOVERRIDES="d3d12,d3d12core=n" WINEDEBUG=-all
(cd "$T/cap" && SM2FIX_REAL_D3D12=real_d3d12.dll SM2FIX_CAPTURE=1 SM2FIX_CAPTURE_AFTER_MS=0 SM2FIX_MODE=off \
    xvfb-run -a wine capture_demo.exe "$EXE")
(cd "$T/rep" && xvfb-run -a wine replay.exe "$EXE" report.txt "$T"/cap/sm2fix-capture/*.bin) | tee "$T/out"
grep -q "RESULT: 1 captures match, 0 differ, 0 couldn't be replayed" "$T/out"
echo "capture self-test passed"
