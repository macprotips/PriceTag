#!/usr/bin/env bash
# End-to-end test under Wine: fake game -> sm2fix d3d12.dll -> mock driver.
set -euo pipefail
cd "$(dirname "$0")/../.."
./build.sh >/dev/null
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then CC=(x86_64-w64-mingw32-gcc)
elif command -v zig >/dev/null 2>&1; then CC=(zig cc -target x86_64-windows-gnu)
else CC=(python3 -m ziglang cc -target x86_64-windows-gnu); fi
OUT=build/e2e
rm -rf "$OUT" && mkdir -p "$OUT"
cp build/d3d12.dll "$OUT/"
"${CC[@]}" -O1 -shared -o "$OUT/mock_d3d12.dll" tests/e2e/mock_d3d12.c tests/e2e/mock_d3d12.def
"${CC[@]}" -O1 -o "$OUT/fake_game.exe" tests/e2e/fake_game.c shim/sm2fix_core.c
export WINEPREFIX="${WINEPREFIX:-$PWD/build/wineprefix}" WINEDEBUG=-all
# Make sure the native shim beats Wine's builtin d3d12 when loaded from the app dir.
export WINEDLLOVERRIDES="d3d12=n,b"
cd "$OUT"
status=0
for mode in hash all off; do
    wine fake_game.exe "$mode" || status=1
done
echo "--- sm2fix.log (last run) ---"
cat sm2fix.log
exit $status
