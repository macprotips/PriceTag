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
"${CC[@]}" -O1 -shared -o "$OUT/mock_d3d12.dll" tests/e2e/mock_d3d12.c shim/sm2fix_dxbc.c tests/e2e/mock_d3d12.def
"${CC[@]}" -O1 -o "$OUT/fake_game.exe" tests/e2e/fake_game.c shim/sm2fix_core.c shim/sm2fix_dxbc.c
export WINEPREFIX="${WINEPREFIX:-$PWD/build/wineprefix}" WINEDEBUG=-all
# Make sure the native shim beats Wine's builtin d3d12 when loaded from the app dir.
export WINEDLLOVERRIDES="d3d12=n,b"
cd "$OUT"
printf '%s' "DXBC-pretend-replacement-skinning-shader-0123456789" > sm2fix-skinning.dxil
status=0
for mode in hash all max off; do
    rm -rf sm2fix-dumps
    wine fake_game.exe "$mode" || status=1
    cp sm2fix.log "sm2fix-$mode.log"
    if [[ $mode != off ]] && ! grep -q "skin launcher (first calls): calls=1 with-requests=1 blocked-by-flag=1 .*+30(requests)=41 .*jobsys flag(+24)=7" sm2fix.log; then
        echo "FAIL: skin probe ($mode)"; grep -i "skin" sm2fix.log; status=1
    fi
    if [[ $mode == hash ]] && ! { grep -q "RenderBudgets (SkinningSB about to be created): ModelInstMax=100 ModelSkinInstMax=200 ModelSkinSubsetMax=300 ModelSkinGroupMax=16 " sm2fix.log \
            && grep -q "skin_budget: raising ModelSkinGroupMax 16 -> 65536" sm2fix.log \
            && grep -q "SkinningSB created: ok=1 capacity=65536 entries, stride=32 (ModelSkinGroupMax was 16)" sm2fix.log \
            && grep -q "exit: skin dispatch calls=4 with-work=4 over-capacity=0 capacity=65536" sm2fix.log; }; then
        echo "FAIL: skin budget ($mode)"; grep -i "skin" sm2fix.log; status=1
    fi
    if [[ $mode == all || $mode == max ]] && ! { grep -q "SkinningSB created: ok=1 capacity=16 entries" sm2fix.log \
            && grep -q "skin dispatch (first calls): calls=1 with-work=1 over-capacity=0 | entries queued=5 SkinningSB capacity=16" sm2fix.log; }; then
        echo "FAIL: skin budget probe ($mode)"; grep -i "skin" sm2fix.log; status=1
    fi
    if [[ $mode == hash ]] && ! grep -q "replace_skinning: created the replacement skinning pipeline: hr=0" sm2fix.log; then
        echo "FAIL: replace_skinning ($mode)"; grep -i "replace" sm2fix.log; status=1
    fi
    if [[ $mode != off ]] && ! grep -q "skin window:   root 1 \[0\] SRV dim=1 res=.* va=0xabc000 width=1048576 heap=2 first=7 num=10 stride=64" sm2fix.log; then
        echo "FAIL: descriptor tracking ($mode)"; grep -i "skin window\|descriptor heap" sm2fix.log; status=1
    fi
    if [[ $mode != off ]] && ! grep -q "skin palettes (as the dispatch is recorded): 5 queued items, 1 skipped (no palette), 2 palettes ANIMATED, 2 IDENTITY" sm2fix.log; then
        echo "FAIL: palette analysis ($mode)"; grep -i "palette" sm2fix.log; status=1
    fi
    if [[ $mode != off ]] && ! grep -q "skin palettes: record flag bits (+4) among IDENTITY: .*(unreadable 2) | among ANIMATED: .*(unreadable 2)" sm2fix.log; then
        echo "FAIL: palette flag breakdown ($mode)"; grep -i "palette" sm2fix.log; status=1
    fi
    if [[ $mode != off ]] && ! grep -q "skin palettes (at queue submission 1, +[0-9]* ms): of 2 identity palettes, 2 still identity, 0 now ANIMATED" sm2fix.log; then
        echo "FAIL: identity palette recheck ($mode)"; grep -i "palette" sm2fix.log; status=1
    fi
    if [[ $mode != off ]] && ! grep -q "skin window:     data: [0-9a-f]\{8\} " sm2fix.log; then
        echo "FAIL: upload-buffer data dump ($mode)"; grep -i "skin window" sm2fix.log; status=1
    fi
    if [[ $mode != off ]] && ! grep -q "force_anim: scanned .*AnimUpdateLODPrius=1 AnimControllerComponentPrius=1 SimpleAnimComponentPrius=[0-9]*, 3 changed now" sm2fix.log; then
        echo "FAIL: force_anim ($mode)"; grep -i "force_anim" sm2fix.log; status=1
    fi
    if [[ $mode != off ]] && ! grep -q "readbacks: \[0\] 4096 bytes, 1 copies (buffer) from src .* (1048576, heap 2): 0.8% non-zero" sm2fix.log; then
        echo "FAIL: readback monitor ($mode)"; grep -i "readback" sm2fix.log; status=1
    fi
    if [[ $mode == off ]] && awk '/new launch/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' sm2fix.log | grep -q "SkinningSB\|skin dispatch"; then
        echo "FAIL: skin hooks active in off mode"; status=1
    fi
    if [[ $mode != all && $mode != max ]] && ! grep -q "timestamp x3 .*t0=1000 deltas(us): 600.0 1000.0" sm2fix.log; then
        echo "FAIL: query tracing ($mode)"; grep -i "quer\|timestamp" sm2fix.log; status=1
    fi
    # Profiler: the target shader was bound for 4 dispatches (SetPipelineState, Reset,
    # stream PSO, bundle) and 1 ExecuteIndirect; the other shader for 1 dispatch.
    target=$(grep -o 'target hash [0-9a-f]*' sm2fix.log | head -1 | cut -d' ' -f3)
    if ! grep -q "profile (exit): 2 compute shaders used" sm2fix.log \
            || ! grep -q "  $target dispatches=4 indirect=1" sm2fix.log; then
        echo "FAIL: profiler output ($mode)"; grep profile -A3 sm2fix.log; status=1
    fi
done
echo "--- sm2fix.log (max run) ---"
cat sm2fix-max.log
exit $status
