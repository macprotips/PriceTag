#!/usr/bin/env bash
# Rebuilds the Rosetta instruction check from the game executable.
#   build.sh <Spider-Man2.exe>   (needs python3 with pefile+capstone, GNU as, zig or mingw, wine)
# The golden results must come from a real x86-64 CPU (run under wine on Linux/Windows).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
python3 "$HERE/harvest.py" "$1" "$HERE"
bash "$HERE/build-tests.sh" "$HERE"
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then CC=(x86_64-w64-mingw32-gcc)
elif command -v zig >/dev/null 2>&1; then CC=(zig cc -target x86_64-windows-gnu)
else CC=(python3 -m ziglang cc -target x86_64-windows-gnu); fi
"${CC[@]}" -O1 -Wall -I"$HERE" -o "$HERE/rosetta_fuzz.exe" "$HERE/rosetta_fuzz.c"
rm -f "$HERE"/*.pdb "$HERE"/tests.o "$HERE"/tests.bin "$HERE"/tests.syms "$HERE"/tests.work.s "$HERE"/as.err "$HERE"/bad.lines
(cd "$HERE" && wine rosetta_fuzz.exe generate golden.bin)
