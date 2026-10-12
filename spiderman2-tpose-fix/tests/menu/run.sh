#!/usr/bin/env bash
# Drives every option of "Spider-Man 2 Fix.command" against a fake home folder, both
# with and without a (fake) CrossOver install, and checks each one did its job.
set -uo pipefail
cd "$(dirname "$0")/../.."
CMD="$PWD/Spider-Man 2 Fix.command"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

check() { # description, command...
    local what="$1"; shift
    if "$@"; then echo "ok   $what"; else echo "FAIL $what"; fails=$((fails + 1)); fi
}

for variant in no-crossover crossover; do
    H="$TMP/$variant"
    G="$H/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Marvel's Spider-Man 2"
    mkdir -p "$G" "$H/Desktop"
    touch "$G/Spider-Man2.exe" "$G/dstorage.dll" "$G/dstoragecore.dll"
    if [[ $variant == crossover ]]; then
        A="$H/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin"
        mkdir -p "$A"
        # Fake wine: records its arguments; for the GPU check it writes a report like the real test.
        printf '#!/bin/bash\nprintf "%%s\\n" "$@" "OVERRIDES=$WINEDLLOVERRIDES" > "%s/wine-args"\ncase "$*" in *bigbuf_test.exe*) echo "RESULT: fake" > bigbuf-report.txt ;; *rosetta_fuzz.exe*) echo "RESULT: fake" > fuzz-report.txt ;; *skin_gpu_test.exe*) echo "RESULT: fake" > skin-report.txt ;; *format_test.exe*) echo "RESULT: fake" > format-report.txt ;; esac\nexit 0\n' "$H" > "$A/wine"
        chmod +x "$A/wine"
    fi
    run() { printf '%s\n\n' "$1" | HOME="$H" TERM=dumb SM2FIX_VOLUMES=/nonexistent bash "$CMD" > "$TMP/out" 2>&1; }
    ini() { grep -q "^$1 = $2\$" "$G/sm2fix.ini"; }
    out() { grep -q "$1" "$TMP/out"; }

    echo "== $variant"
    run 1;  check "1 test#28 THE FIX"              eval 'ini pool_fix 1 && ini mode off && ini probe_skin 0 && ini capture 0 && ini replace_skinning 0 \
            && [ -f "$G/d3d12.dll" ] && [ -f "$G/sm2fix-pool-3d50acdb571dce22.dxil" ] && [ -f "$G/sm2fix-pool-14902794dd7134e4.dxil" ] \
            && out "Test #28 installed"'
    run 10; check "fix shows as installed"         eval 'out "Currently installed: Test #28 THE FIX"'
    run 31; check "31 test#25 capture all"          eval 'ini capture 1 && ini capture_all 1 && ini pool_fix 0 && [ ! -f "$G/sm2fix-pool-3d50acdb571dce22.dxil" ]'
    run 1;  check "1 again"                        eval 'ini pool_fix 1'
    run 4;  check "4 remove takes the pool shaders too" eval '! ls "$G"/sm2fix-pool-*.dxil >/dev/null 2>&1 && [ ! -f "$G/d3d12.dll" ]'
    run 31; check "31 test#25 capture all"          eval 'ini capture 1 && ini capture_all 1 && ini capture_view_mb 8 && ini capture_budget_mb 250 && ini mode off && [ -f "$G/d3d12.dll" ]'
    run 30; check "30 test#24 ziva capture"        eval 'ini capture 1 && ini capture_all 0 && ini capture_view_mb 32 && ini mode off && ini ziva_cpu auto && ini probe_skin 0'
    mkdir -p "$G/sm2fix-capture" && echo "x" > "$G/sm2fix.log"
    for n in 1 2 3; do head -c 70000000 /dev/zero > "$G/sm2fix-capture/c$n.bin"; done
    run 3;  check "3 save results zips captures in parts" eval 'ls "$H"/Desktop/sm2fix-capture-*/part2.zip >/dev/null 2>&1 \
            && true && [ ! -e "$H"/Desktop/sm2fix-capture-*/part3.zip ]'
    run 29; check "29 test#23 ziva reference"       eval 'ini ziva_cpu reference && ini mode off && ini probe_skin 0 && ini capture 0 && [ -f "$G/d3d12.dll" ]'
    run 27; check "27 test#23b ziva sse41"         eval 'ini ziva_cpu sse41 && ini mode off'
    run 28; check "28 test#23c ziva avx2"          eval 'ini ziva_cpu avx2 && ini mode off'
    run 25; check "25 test#21 writer trap"         eval 'ini trap_writer 1 && ini probe_skin 1 && ini force_anim 0 && ini ziva_cpu auto'
    run 24; check "24 test#20 readback watch"      eval 'ini probe_skin 1 && ini trap_writer 0 && ini force_anim 0'
    run 23; check "23 test#19 force anim"          eval 'ini force_anim 1 && ini probe_skin 1 && ini skip_skinning 0'
    run 22; check "22 test#18 inputs trace"        eval 'ini probe_skin 1 && ini replace_skinning 0 && ini skip_skinning 0 && ini profile 1'
    run 21; check "21 test#17 corrected shader"    eval 'ini replace_skinning 1 && ini skip_skinning 0 && [ -f "$G/sm2fix-skinning.dxil" ] && [ -f "$G/d3d12.dll" ]'
    run 4;  check "4 remove takes the shader too"  eval '[ ! -f "$G/sm2fix-skinning.dxil" ] && [ ! -f "$G/d3d12.dll" ]'
    run 19; check "19 test#15 skip skinning"       eval 'ini skip_skinning 1 && ini probe_skin 1 && ini profile 1 && [ -f "$G/d3d12.dll" ]'
    run 18; check "18 test#14 skin trace"          eval 'ini probe_skin 1 && ini skin_budget 0 && ini skip_skinning 0'
    run 17; check "17 test#13 skin budget"         eval 'ini skin_budget 65536 && ini probe_skin 1'
    run 16; check "16 test#11 fake timestamps"     eval 'ini fake_timestamps 1 && ini probe_skin 0'
    run 15; check "15 test#10 tracer only"         eval 'ini trace_queries 1 && ini fake_timestamps 0'
    run 6;  check "6 test#1 barrier fix"           eval 'ini mode all'
    run 5;  check "5 test#3 deep test"             eval 'ini mode max'
    run 9;  check "9 test#4b spoof"                eval 'ini spoof no_int64,no_fp16'
    run 12; check "12 test#5 sampler fix"          eval 'ini minmax linear && ini dump_graphics 1'
    run 11; check "11 test#6 tracker"              eval 'ini profile 1 && ini cpu_count 0'
    run 13; check "13 test#7 cpu cores"            eval 'ini cpu_count 8'
    run 14; check "14 test#8 slow loading"         eval 'ini pso_delay_ms 10'
    run 8;  check "8 faster version"               eval 'ini mode hash'
    echo "D3D12CreateDevice ok" > "$G/sm2fix.log"
    run 3;  check "3 save results"                 eval 'ls "$H"/Desktop/sm2fix-log-*.txt >/dev/null 2>&1'
    run 7;  check "7 DirectStorage off"            eval '[ -f "$G/dstorage.dll.sm2fix-off" ] && [ ! -f "$G/d3d12.dll" ]'
    run 4;  check "4 remove everything"            eval '[ ! -f "$G/d3d12.dll" ] && [ -f "$G/dstorage.dll" ] && [ ! -f "$G/sm2fix.ini" ]'
    run 10; check "10 quit changes nothing"        eval 'out "Nothing changed"'
    if [[ $variant == crossover ]]; then
        run 19; check "override set via CrossOver wine" eval 'out "Set per-game DLL override"'
        rm -f "$H"/Desktop/skin-report-*.txt
        run 2; check "2 test#16 GPU check runs in the bottle" eval 'ls "$H"/Desktop/skin-report-*.txt >/dev/null 2>&1 \
            && grep -qx -- "--bottle" "$H/wine-args" && grep -qx "skin_gpu_test.exe" "$H/wine-args" && grep -q "sm2fix-skinning.dxil$" "$H/wine-args" && grep -qx "OVERRIDES=winemac.drv=d" "$H/wine-args" \
            && grep -q "^Z:.*\\\\Marvel'"'"'s Spider-Man 2\\\\Spider-Man2.exe$" "$H/wine-args"'
        rm -f "$H"/Desktop/cpu-report-*.txt
        rm -f "$H"/Desktop/bigbuf-report-*.txt
        run 33; check "33 test#27 big-buffer check runs in the bottle" eval 'ls "$H"/Desktop/bigbuf-report-2*.txt >/dev/null 2>&1 \
            && grep -qx "bigbuf_test.exe" "$H/wine-args" && grep -qx "OVERRIDES=winemac.drv=d" "$H/wine-args"'
        run 32; check "32 test#26 cpu check runs in the bottle" eval 'ls "$H"/Desktop/cpu-report-2*.txt >/dev/null 2>&1 \
            && grep -qx "rosetta_fuzz.exe" "$H/wine-args" && grep -qx "check" "$H/wine-args" && grep -qx "OVERRIDES=winemac.drv=d" "$H/wine-args"'
        rm -f "$H"/Desktop/format-report-*.txt
        run 26; check "26 test#22 format check runs in the bottle" eval 'ls "$H"/Desktop/format-report-*.txt >/dev/null 2>&1 \
            && grep -qx -- "--bottle" "$H/wine-args" && grep -qx "format_test.exe" "$H/wine-args" && grep -qx "formats" "$H/wine-args" && grep -qx "OVERRIDES=winemac.drv=d" "$H/wine-args"'
    else
        run 19; check "manual override hint without CrossOver" eval 'out "Do it once by hand"'
    fi
done

echo "$fails failure(s)"
exit $((fails != 0))
