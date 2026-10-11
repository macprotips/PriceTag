#!/usr/bin/env bash
# Builds skin_gpu_test.exe and format_test.exe (Windows x86_64, needs zig or mingw).
set -euo pipefail
cd "$(dirname "$0")"
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then CC=(x86_64-w64-mingw32-gcc)
elif command -v zig >/dev/null 2>&1; then CC=(zig cc -target x86_64-windows-gnu)
else CC=(python3 -m ziglang cc -target x86_64-windows-gnu); fi
"${CC[@]}" -O2 -Wall -Wno-unused-function -o skin_gpu_test.exe skin_gpu_test.c skin_inputs.c
"${CC[@]}" -O2 -Wall -o format_test.exe format_test.c
rm -f skin_gpu_test.pdb format_test.pdb
echo "built skin_gpu_test.exe format_test.exe"
