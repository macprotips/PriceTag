#!/usr/bin/env bash
# Builds build/d3d12.dll (diagnostic shim) and build/fix/d3d12.dll (the fix), Windows x86_64. Needs either a MinGW-w64 cross compiler
# (brew install mingw-w64 / apt install mingw-w64) or Zig (brew install zig, or
# pip install ziglang). Output: build/d3d12.dll
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

SRC=(shim/sm2fix.c shim/sm2fix_core.c shim/sm2fix_dxbc.c shim/sm2fix_cpu.c)
FLAGS=(-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -shared)

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    CC=(x86_64-w64-mingw32-gcc)
elif command -v zig >/dev/null 2>&1; then
    CC=(zig cc -target x86_64-windows-gnu)
elif python3 -c "import ziglang" >/dev/null 2>&1; then
    CC=(python3 -m ziglang cc -target x86_64-windows-gnu)
else
    echo "Need x86_64-w64-mingw32-gcc or zig. Try: brew install mingw-w64" >&2
    exit 1
fi

if [[ "${CC[0]}" == x86_64-w64-mingw32-gcc ]]; then
    FLAGS+=(-static-libgcc)
fi
"${CC[@]}" "${FLAGS[@]}" -o build/d3d12.dll "${SRC[@]}" shim/d3d12.def
cp shim/sm2fix.ini build/sm2fix.ini
echo "built build/d3d12.dll (the diagnostic shim used by the troubleshooting menu)"

# The fix itself, as installed by the Mac app: shim/fix.c only.
mkdir -p build/fix
"${CC[@]}" "${FLAGS[@]}" -o build/fix/d3d12.dll shim/fix.c shim/sm2fix_core.c shim/fix.def
echo "built build/fix/d3d12.dll (the fix)"
