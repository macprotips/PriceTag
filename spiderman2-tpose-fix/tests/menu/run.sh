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
        printf '#!/bin/bash\nexit 0\n' > "$A/wine"
        chmod +x "$A/wine"
    fi
    run() { printf '%s\n\n' "$1" | HOME="$H" TERM=dumb SM2FIX_VOLUMES=/nonexistent bash "$CMD" > "$TMP/out" 2>&1; }
    ini() { grep -q "^$1 = $2\$" "$G/sm2fix.ini"; }
    out() { grep -q "$1" "$TMP/out"; }

    echo "== $variant"
    run 1;  check "1 test#12 skin probe"           eval 'ini probe_skin 1 && ini profile 1 && [ -f "$G/d3d12.dll" ]'
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
        run 1; check "override set via CrossOver wine" eval 'out "Set per-game DLL override"'
    else
        run 1; check "manual override hint without CrossOver" eval 'out "Do it once by hand"'
    fi
done

echo "$fails failure(s)"
exit $((fails != 0))
