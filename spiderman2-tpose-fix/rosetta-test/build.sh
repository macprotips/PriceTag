#!/usr/bin/env bash
# Builds rosetta_test.exe (Windows x86_64, needs zig or mingw) and, if Wine is available
# on a real x86 CPU with AVX2/FMA/F16C, generates golden.txt from it.
set -euo pipefail
cd "$(dirname "$0")"
FLAGS=(-O1 -std=gnu11 -mavx2 -mfma -mf16c -mbmi -mbmi2 -mlzcnt -mpopcnt -msse4.2 -mcx16 -Wall -Wno-unused-function)
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then CC=(x86_64-w64-mingw32-gcc)
elif command -v zig >/dev/null 2>&1; then CC=(zig cc -target x86_64-windows-gnu)
else CC=(python3 -m ziglang cc -target x86_64-windows-gnu); fi
"${CC[@]}" "${FLAGS[@]}" -o rosetta_test.exe rosetta_test.c
echo "built rosetta_test.exe"
if [[ "${1:-}" == golden ]]; then
    WINEDEBUG=-all wine rosetta_test.exe generate golden.txt
    WINEDEBUG=-all wine rosetta_test.exe check golden.txt self-check.txt
    grep -E "differ|failed" self-check.txt
fi
