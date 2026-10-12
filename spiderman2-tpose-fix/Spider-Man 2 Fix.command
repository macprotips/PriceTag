#!/bin/bash
# Double-click me in Finder. Finds Spider-Man 2 in your CrossOver / Whisky bottles and
# installs, tunes or removes the T-pose fix. Safe to run as many times as you like.
# (Written for the bash 3.2 that ships with macOS.)

HERE="$(cd "$(dirname "$0")" && pwd)"
INSTALLER="$HERE/scripts/install-macos.sh"
CX_WINE="$(bash "$(dirname "$0")/scripts/find-crossover-wine.sh" 2>/dev/null || true)"
WHISKY_WINE="$HOME/Library/Application Support/com.isaacmarovitz.Whisky/Libraries/Wine/bin/wine64"

say()  { printf '%s\n' "$*"; }
line() { say "------------------------------------------------------------"; }
pause_exit() { say ""; read -r -p "Press Return to close this window." _; exit "${1:-0}"; }

clear
line
say "  Spider-Man 2 T-pose fix (for M1 / M2 / M3 / M4 Macs)"
line
say ""
say "Quit Spider-Man 2 before continuing."
# A game (or leftover Wine process) still running keeps the log open, and some
# external-drive filesystems then block deleting it, which looks like a hang.
game_running() {
    # The game process itself, not shells/scripts that merely mention its name.
    ps -axo pid=,args= 2>/dev/null | grep 'Spider-Man2\.exe' \
        | grep -vE 'grep|bash|/sh |zsh|python|\.command|install-macos' | grep -q .
}
while game_running; do
    say ""
    say "Spider-Man 2 still seems to be running (maybe in the background)."
    say "Quit it, or quit CrossOver completely (CrossOver menu > Quit CrossOver),"
    read -r -p "then press Return to check again (or type s to skip): " ans || break
    [ "$ans" = s ] && break
done
say ""
say "Looking for Spider-Man 2..."

VOLUMES="${SM2FIX_VOLUMES:-/Volumes}"   # overridable for testing
GAME_SUBDIR="steamapps/common/Marvel's Spider-Man 2"

# Every Wine bottle on this Mac, as: kind|name|path
BOTTLES=()
add_bottles() { # kind, bottles dir
    local b
    [ -d "$2" ] || return
    for b in "$2"/*; do
        [ -d "$b/drive_c" ] && BOTTLES+=("$1|$(basename "$b")|$b")
    done
}
add_bottles CrossOver "$HOME/Library/Application Support/CrossOver/Bottles"
add_bottles Whisky "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Bottles"

# Each match: kind|bottle name|bottle path|game folder
FOUND=()
add_found() {
    local f
    for f in "${FOUND[@]}"; do [ "$f" = "$1" ] && return; done
    FOUND+=("$1")
}

# Turns a Windows path from Steam's libraryfolders.vdf (e.g. Z:\Volumes\MPT\SteamLibrary)
# into a Mac path, using the bottle's drive-letter links.
win_to_mac() { # bottle, windows path
    local letter rest root
    letter="$(printf '%s' "${2:0:1}" | tr 'A-Z' 'a-z')"
    rest="$(printf '%s' "${2:2}" | tr '\\' '/')"
    root="$(cd -P "$1/dosdevices/$letter:" 2>/dev/null && pwd)" || return 1
    printf '%s%s' "${root%/}" "$rest"
}

for entry in "${BOTTLES[@]}"; do
    IFS='|' read -r kind name bottle <<< "$entry"
    exe="$(find "$bottle/drive_c" -name 'Spider-Man2.exe' -not -path '*/windows/*' 2>/dev/null | head -n 1)"
    [ -n "$exe" ] && add_found "$kind|$name|$bottle|$(dirname "$exe")"
    # Steam libraries on other drives (external disks etc.)
    for vdf in "$bottle/drive_c/Program Files (x86)/Steam/steamapps/libraryfolders.vdf" \
               "$bottle/drive_c/Program Files/Steam/steamapps/libraryfolders.vdf"; do
        [ -f "$vdf" ] || continue
        sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | sed 's/\\\\/\\/g' |
        while IFS= read -r winpath; do
            lib="$(win_to_mac "$bottle" "$winpath")" || continue
            [ -f "$lib/$GAME_SUBDIR/Spider-Man2.exe" ] && echo "$lib/$GAME_SUBDIR"
        done > "${TMPDIR:-/tmp}/sm2fix-libs.$$"
        while IFS= read -r dir; do
            add_found "$kind|$name|$bottle|$dir"
        done < "${TMPDIR:-/tmp}/sm2fix-libs.$$"
        rm -f "${TMPDIR:-/tmp}/sm2fix-libs.$$"
    done
done

# Asks which bottle runs Steam when we found the game but not its bottle.
pick_bottle_for() { # game dir
    local steam=() e kind name bottle i n
    for e in "${BOTTLES[@]}"; do
        IFS='|' read -r kind name bottle <<< "$e"
        { [ -f "$bottle/drive_c/Program Files (x86)/Steam/steam.exe" ] || [ -f "$bottle/drive_c/Program Files/Steam/steam.exe" ]; } && steam+=("$e")
    done
    [ ${#steam[@]} -eq 0 ] && steam=("${BOTTLES[@]}")
    if [ ${#steam[@]} -eq 0 ]; then
        add_found "Other|none||$1"
        return
    fi
    n=1
    if [ ${#steam[@]} -gt 1 ]; then
        say ""
        say "Which bottle do you run Steam / Spider-Man 2 from?"
        i=1
        for e in "${steam[@]}"; do
            IFS='|' read -r kind name _ <<< "$e"
            say "  $i) $kind bottle \"$name\""
            i=$((i + 1))
        done
        read -r -p "Type a number and press Return: " n
        case "$n" in ''|*[!0-9]*) n=1 ;; esac
        { [ "$n" -ge 1 ] && [ "$n" -le ${#steam[@]} ]; } || n=1
    fi
    IFS='|' read -r kind name bottle <<< "${steam[$((n - 1))]}"
    add_found "$kind|$name|$bottle|$1"
}

if [ ${#FOUND[@]} -eq 0 ]; then
    # Steam libraries on external drives that no bottle points at directly.
    for dir in "$VOLUMES"/*/"$GAME_SUBDIR" "$VOLUMES"/*/*/"$GAME_SUBDIR" "$VOLUMES"/*/*/*/"$GAME_SUBDIR"; do
        if [ -f "$dir/Spider-Man2.exe" ]; then
            pick_bottle_for "$dir"
            break
        fi
    done
fi

if [ ${#FOUND[@]} -eq 0 ]; then
    say ""
    say "I couldn't find Spider-Man 2 automatically."
    say "Drag Spider-Man2.exe (or its folder) onto this window, then press Return:"
    read -r -p "> " GAME_DIR
    # Undo Terminal's drag-and-drop escaping (\ before spaces, quotes, etc.).
    GAME_DIR="$(printf '%s' "$GAME_DIR" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
        -e "s/^'\(.*\)'\$/\1/" -e 's/^"\(.*\)"$/\1/' -e 's/\\\(.\)/\1/g')"
    [ -f "$GAME_DIR" ] && GAME_DIR="$(dirname "$GAME_DIR")"
    GAME_DIR="${GAME_DIR%/}"
    if [ ! -f "$GAME_DIR/Spider-Man2.exe" ]; then
        say "Couldn't find Spider-Man2.exe at: $GAME_DIR"
        pause_exit 1
    fi
    pick_bottle_for "$GAME_DIR"
fi

CHOICE="${FOUND[0]}"
if [ ${#FOUND[@]} -gt 1 ]; then
    say ""
    say "Found Spider-Man 2 in more than one place. Which one do you play?"
    i=1
    for f in "${FOUND[@]}"; do
        IFS='|' read -r kind name _ dir <<< "$f"
        say "  $i) $dir  ($kind bottle \"$name\")"
        i=$((i + 1))
    done
    read -r -p "Type a number and press Return: " n
    case "$n" in ''|*[!0-9]*) n=1 ;; esac
    { [ "$n" -ge 1 ] && [ "$n" -le ${#FOUND[@]} ]; } || n=1
    CHOICE="${FOUND[$((n - 1))]}"
fi
IFS='|' read -r KIND NAME PREFIX GAME_DIR <<< "$CHOICE"

ARGS=(--game-dir "$GAME_DIR")
if [ "$KIND" = CrossOver ]; then
    ARGS+=(--bottle "$NAME")
elif [ "$KIND" = Whisky ] && [ -x "$WHISKY_WINE" ]; then
    ARGS+=(--prefix "$PREFIX" --wine "$WHISKY_WINE")
elif [ -n "$PREFIX" ]; then
    ARGS+=(--prefix "$PREFIX")
fi

current_mode() {
    if [ -f "$GAME_DIR/d3d12.dll" ] && grep -q sm2fix "$GAME_DIR/d3d12.dll" 2>/dev/null; then
        sed -n 's/^mode *= *//p' "$GAME_DIR/sm2fix.ini" 2>/dev/null | head -n 1
    fi
}

mode_label() {
    if grep -q '^pool_fix = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #28 THE FIX (big-buffer fix)"
        return
    fi
    if grep -q '^trap_writer = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #21 animation code trap"
        return
    fi
    if grep -q '^force_anim = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #19 always animate (possible FIX)"
        return
    fi
    if grep -q '^replace_skinning = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #17 corrected skinning shader"
        return
    fi
    if grep -q '^skip_skinning = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #15 skinning shader switched off"
        return
    fi
    if grep -q '^skin_budget = [1-9]' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #13 bigger skinning buffer"
        return
    fi
    if grep -q '^probe_skin = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #20 GPU readback watch"
        return
    fi
    if grep -q '^fake_timestamps = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #11 real GPU clock (timestamp fix)"
        return
    fi
    if grep -q '^trace_queries = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #10 GPU query tracer"
        return
    fi
    if grep -q '^pso_delay_ms = [1-9]' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #8 slow loading ($(sed -n 's/^pso_delay_ms = //p' "$GAME_DIR/sm2fix.ini") ms)"
        return
    fi
    if grep -q '^cpu_count = [1-9]' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #7 fewer CPU cores ($(sed -n 's/^cpu_count = //p' "$GAME_DIR/sm2fix.ini"))"
        return
    fi
    if grep -q '^profile = 1' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #6 animation tracker"
        return
    fi
    if grep -q '^minmax = \(linear\|point\)' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #5 sampler fix ($(sed -n 's/^minmax = //p' "$GAME_DIR/sm2fix.ini"))"
        return
    fi
    if grep -q '^spoof = [a-z]' "$GAME_DIR/sm2fix.ini" 2>/dev/null; then
        echo "Test #4 hide GPU features ($(sed -n 's/^spoof = //p' "$GAME_DIR/sm2fix.ini"))"
        return
    fi
    case "$1" in
        max) echo "Test #3 deep test" ;;
        all) echo "Test #1 barrier fix" ;;
        hash) echo "barrier fix, fast version" ;;
        off) echo "log only" ;;
        *) echo "$1" ;;
    esac
}

ds_off() {
    local f
    for f in dstorage.dll dstoragecore.dll; do
        [ -f "$GAME_DIR/$f" ] && mv "$GAME_DIR/$f" "$GAME_DIR/$f.sm2fix-off"
    done
}
ds_on() {
    local f
    for f in dstorage.dll dstoragecore.dll; do
        [ -f "$GAME_DIR/$f.sm2fix-off" ] && mv "$GAME_DIR/$f.sm2fix-off" "$GAME_DIR/$f"
    done
}

say ""
say "Found it: $GAME_DIR"
[ "$KIND" != Other ] && say "Bottle:   $KIND \"$NAME\""
DS_OFF=0
{ [ -f "$GAME_DIR/dstorage.dll.sm2fix-off" ] || [ -f "$GAME_DIR/dstoragecore.dll.sm2fix-off" ]; } && DS_OFF=1
MODE_NOW="$(current_mode)"
if [ -n "$MODE_NOW" ]; then
    say "Currently installed: $(mode_label "$MODE_NOW")"
else
    say "Currently installed: nothing"
fi
[ $DS_OFF -eq 1 ] && say "DirectStorage is turned OFF (test #2)."

say ""
say "What do you want to do?"
say "  1) Test #28: THE FIX for the T-pose (install, then play)  <-- do this one"
say "  2) Test #16: GPU check (done: the shader works on your Mac)"
say "  3) Save my results to the Desktop (to send for help)"
say "  4) Remove everything (back to normal)"
say ""
say "  Older tests:"
say "  5) Test #3: deep test"
say "  6) Test #1: barrier fix"
say "  7) Test #2: DirectStorage off"
say "  8) A test fixed it! Switch to the faster version"
say "  9) Test #4: hide GPU features"
say "  11) Test #6: animation tracker"
say "  12) Test #5: sampler fix"
say "  13) Test #7: game sees 8 CPU cores"
say "  14) Test #8: slow loading"
say "  15) Test #10: GPU query tracer"
say "  16) Test #11: real GPU clock"
say "  17) Test #13: bigger skinning buffer (done: budget was fine)"
say "  18) Test #14: skinning trace (done: the shader does run)"
say "  19) Test #15: skinning shader off (done: its output is what you see)"
say "  20) Test #9: Rosetta instruction check (done: Rosetta is fine)"
say "  21) Test #17: rewritten skinning shader (done: not needed, shader is fine)"
say "  22) Test #18: skinning inputs trace (done: some bone sets are never written)"
say "  23) Test #19: always animate (done: didn't help)"
say "  24) Test #20: GPU readback watch (done: readbacks are fine)"
say "  25) Test #21: catch the code that animates characters (done: only caught memory clears)"
say "  26) Test #22: GPU data-format check (done: all 40 formats fine)"
say "  33) Test #27: GPU check with a huge (2 GB) buffer (done: found the cause!)"
say "  32) Test #26: check every CPU instruction the game uses (done: Rosetta is correct)"
say "  31) Test #25: check every GPU program the game runs (done: no differences found)"
say "  30) Test #24: record Ziva face/cloth GPU work (done: Ziva GPU math is correct)"
say "  29) Test #23: Ziva face/cloth code, plain version (done: no change)"
say "  27) Test #23b: Ziva face/cloth code, SSE4.1 version (done: no change)"
say "  28) Test #23c: Ziva face/cloth code, AVX2 version (done: no change)"
say "  10) Quit"
read -r -p "Type a number and press Return: " action
say ""

# Map the new menu numbers onto the handlers below.
case "$action" in
    1) action=t28 ;;
    33) action=t27 ;;
    32) action=t26 ;;
    31) action=t25 ;;
    30) action=t24 ;;
    29) action=t23 ;;
    26) action=t22 ;;
    27) action=t23b ;;
    28) action=t23c ;;
    25) action=t21 ;;
    24) action=t20 ;;
    23) action=t19 ;;
    22) action=t18 ;;
    19) action=t15 ;;
    17) action=t13 ;;
    18) action=t14 ;;
    16) action=t11 ;;
    15) action=t10 ;;
    2) action=t16 ;;
    21) action=t17 ;;
    20) action=t9 ;;
    14) action=t8 ;;
    13) action=t7 ;;
    11) action=t6 ;;
    12) action=t5 ;;
    9) action=spoof_b ;;
    3) action=2 ;;
    4) action=3 ;;
    5) action=1 ;;
    6) action=4 ;;
    7) action=5 ;;
    8) action=6 ;;
    *) action=quit ;;
esac

pick_wine() {
    if [ "$KIND" = CrossOver ] && [ -x "$CX_WINE" ]; then
        RUN=("$CX_WINE" --bottle "$NAME")
    elif [ "$KIND" = Whisky ] && [ -x "$WHISKY_WINE" ]; then
        export WINEPREFIX="$PREFIX"
        RUN=("$WHISKY_WINE")
    else
        say "I couldn't find CrossOver automatically."
        say "Drag the CrossOver app (from Finder) onto this window, then press Return:"
        read -r -p "> " APP
        APP="$(printf '%s' "$APP" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
            -e "s/^'\(.*\)'\$/\1/" -e 's/\\\(.\)/\1/g')"
        APP="${APP%/}"
        CX_WINE="$(find "$APP/Contents" -maxdepth 6 -path '*/bin/wine' -type f 2>/dev/null | head -n 1)"
        if [ -z "$CX_WINE" ]; then
            say "No wine found inside '$APP'. Send me a screenshot of this window."
            pause_exit 1
        fi
        RUN=("$CX_WINE" --bottle "$NAME")
    fi
}

case "$action" in
    t28)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps" "$GAME_DIR/sm2fix-capture"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --pool-fix || pause_exit 1
        say ""
        line
        say "Test #28 installed: THE FIX."
        say "Your big-buffer report found it: on M1-M4 Macs, the GPU returns 0 when a"
        say "shader reads a \"typed\" buffer past position 268,435,456. The game keeps all"
        say "character mesh data in one 2 GB buffer and reads it exactly that way, so"
        say "characters stored in its upper half never get their animation (T-pose)."
        say "This fix makes the game read that buffer the \"raw\" way, which your report"
        say "showed works all the way to the end."
        say "  1. Start the game, load your save, walk around near people."
        say "     Are the T-poses / missing heads gone?"
        say "  2. Quit from the in-game menu, run this again, pick 3, and send me the log"
        say "     (and a screenshot either way)."
        line
        ;;
    t25)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps" "$GAME_DIR/sm2fix-capture"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --capture-all || pause_exit 1
        say ""
        line
        say "Test #25 installed. Ziva's GPU work turned out to be correct on your Mac."
        say "This records one job of EVERY GPU compute program the game runs (a few"
        say "hundred), so I can redo all of them on a reference driver and find any that"
        say "your Mac gets wrong. The game will freeze for several seconds once, about"
        say "a minute in, while it records. That's expected."
        say "  1. Start the game, load your save, and play for ~3 minutes where people"
        say "     T-pose (walk around near NPCs)."
        say "  2. Quit from the in-game menu, run this again, pick 3, and send me BOTH"
        say "     files (the log and the capture zip; the zip can be a few hundred MB)."
        line
        ;;
    t24)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps" "$GAME_DIR/sm2fix-capture"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --capture 1 || pause_exit 1
        say ""
        line
        say "Test #24 installed. Faces and clothes are bent by Ziva RT's GPU programs."
        say "This saves one real job of each of them (what went in, what your Mac's GPU"
        say "made of it) so I can redo the same job on a reference GPU driver and see"
        say "whether your Mac got it wrong. The game may pause for a moment once."
        say "  1. Start the game and stay ~2 minutes on the main menu (the one where"
        say "     Peter and Miles have no heads), then load your save for a minute."
        say "  2. Quit from the in-game menu, run this again, pick 3, and send me BOTH"
        say "     files it saves to your Desktop (the log and the capture zip)."
        line
        ;;
    t23|t23b|t23c)
        case "$action" in t23) ZV=reference; ZN="plain (reference)" ;; t23b) ZV=sse41; ZN="SSE4.1" ;; *) ZV=avx2; ZN="AVX2" ;; esac
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --ziva-cpu "$ZV" || pause_exit 1
        say ""
        line
        say "Test #23 installed ($ZN version). Faces and clothes are moved by Ziva RT,"
        say "which has three CPU versions; the game picks one from what your CPU (via"
        say "Rosetta) says it supports. This makes it use the $ZN version instead."
        say "  1. Start the game. Look at the main menu (Peter/Miles' heads) and then"
        say "     people in the world. Is the T-pose / missing heads gone?"
        say "  2. Quit from the in-game menu, run this again, pick 3, send the log"
        say "     (and a screenshot if you can)."
        line
        ;;
    t27)
        TDIR="$HERE/gpu-test"
        pick_wine
        say "Using: $CX_WINE"
        say "Running the big-buffer GPU check: one 2 GB buffer, read at very large"
        say "positions the way the game reads its character data. About a minute;"
        say "it doesn't change anything. Please wait..."
        say ""
        rm -f "$TDIR/bigbuf-report.txt"
        for TRY in "winemac.drv=d" ""; do
            (cd "$TDIR" && WINEDEBUG=-all WINEDLLOVERRIDES="$TRY" "${RUN[@]}" bigbuf_test.exe bigbuf-report.txt) \
                > "$TDIR/wine-output.txt" 2>&1 &
            FPID=$!
            for _ in $(seq 300); do kill -0 "$FPID" 2>/dev/null || break; sleep 1; done
            kill "$FPID" 2>/dev/null
            wait "$FPID" 2>/dev/null
            grep -q "^RESULT" "$TDIR/bigbuf-report.txt" 2>/dev/null && break
            [ -n "$TRY" ] && say "First attempt didn't finish, trying again with the normal Wine setup..."
        done
        if grep -q "^RESULT" "$TDIR/bigbuf-report.txt" 2>/dev/null; then
            OUT="$HOME/Desktop/bigbuf-report-$(date +%Y%m%d-%H%M).txt"
            {
                echo "mac: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / macOS $(sw_vers -productVersion 2>/dev/null)"
                echo "bottle: $KIND / $NAME"
                cat "$TDIR/bigbuf-report.txt"
            } > "$OUT"
            say ""
            grep "^RESULT" "$TDIR/bigbuf-report.txt"
            say ""
            say "Saved: $OUT"
            say "Send me that file."
            open -R "$OUT" 2>/dev/null
        else
            OUT="$HOME/Desktop/bigbuf-report-FAILED-$(date +%Y%m%d-%H%M).txt"
            { echo "wine: $CX_WINE"; cat "$TDIR/bigbuf-report.txt" 2>/dev/null; echo "--- wine output (last 60 lines)"; \
              tail -n 60 "$TDIR/wine-output.txt" 2>/dev/null; } > "$OUT"
            say "The test didn't finish. Saved what happened to: $OUT"
            say "Send me that file."
        fi
        ;;
    t26)
        TDIR="$HERE/rosetta-fuzz"
        pick_wine
        say "Using: $CX_WINE"
        say "Running the CPU instruction check: every kind of instruction the game's code"
        say "uses (about 1,200) plus 1,900 flag pairs, each on 96 inputs, vs a real Intel CPU."
        say "It takes a minute or two and doesn't change anything. Please wait..."
        say ""
        rm -f "$TDIR/fuzz-report.txt"
        for TRY in "winemac.drv=d" ""; do
            (cd "$TDIR" && WINEDEBUG=-all WINEDLLOVERRIDES="$TRY" "${RUN[@]}" rosetta_fuzz.exe check golden.bin \
                fuzz-report.txt) > "$TDIR/wine-output.txt" 2>&1 &
            FPID=$!
            # never wait more than 10 minutes
            for _ in $(seq 600); do kill -0 "$FPID" 2>/dev/null || break; sleep 1; done
            kill "$FPID" 2>/dev/null
            wait "$FPID" 2>/dev/null
            grep -q "^RESULT" "$TDIR/fuzz-report.txt" 2>/dev/null && break
            [ -n "$TRY" ] && say "First attempt didn't finish, trying again with the normal Wine setup..."
        done
        if grep -q "^RESULT" "$TDIR/fuzz-report.txt" 2>/dev/null; then
            OUT="$HOME/Desktop/cpu-report-$(date +%Y%m%d-%H%M).txt"
            {
                echo "mac: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / macOS $(sw_vers -productVersion 2>/dev/null)"
                echo "bottle: $KIND / $NAME"
                cat "$TDIR/fuzz-report.txt"
            } > "$OUT"
            say ""
            grep "^RESULT" "$TDIR/fuzz-report.txt"
            say ""
            say "Saved: $OUT"
            say "Send me that file."
            open -R "$OUT" 2>/dev/null
        else
            OUT="$HOME/Desktop/cpu-report-FAILED-$(date +%Y%m%d-%H%M).txt"
            { echo "wine: $CX_WINE"; cat "$TDIR/fuzz-report.txt" 2>/dev/null; echo "--- wine output (last 60 lines)"; \
              tail -n 60 "$TDIR/wine-output.txt" 2>/dev/null; } > "$OUT"
            say "The test didn't finish. Saved what happened to: $OUT"
            say "Send me that file."
        fi
        ;;
    t22)
        TDIR="$HERE/gpu-test"
        pick_wine
        say "Using: $CX_WINE"
        say "Running the GPU data-format check. It takes under a minute and"
        say "doesn't change anything. Please wait..."
        say ""
        rm -f "$TDIR/format-report.txt"
        for TRY in "winemac.drv=d" ""; do
            (cd "$TDIR" && WINEDEBUG=-all WINEDLLOVERRIDES="$TRY" "${RUN[@]}" format_test.exe formats \
                format-report.txt) > "$TDIR/wine-output.txt" 2>&1
            grep -q "^RESULT" "$TDIR/format-report.txt" 2>/dev/null && break
            [ -n "$TRY" ] && say "First attempt didn't finish, trying again with the normal Wine setup..."
        done
        if grep -q "^RESULT" "$TDIR/format-report.txt" 2>/dev/null; then
            OUT="$HOME/Desktop/format-report-$(date +%Y%m%d-%H%M).txt"
            {
                echo "mac: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / macOS $(sw_vers -productVersion 2>/dev/null)"
                echo "bottle: $KIND / $NAME"
                cat "$TDIR/format-report.txt"
            } > "$OUT"
            say ""
            grep "^RESULT" "$TDIR/format-report.txt"
            say ""
            say "Saved: $OUT"
            say "Send me that file."
            open -R "$OUT" 2>/dev/null
        else
            OUT="$HOME/Desktop/format-report-FAILED-$(date +%Y%m%d-%H%M).txt"
            { echo "wine: $CX_WINE"; cat "$TDIR/format-report.txt" 2>/dev/null; echo "--- wine output (last 60 lines)"; \
              tail -n 60 "$TDIR/wine-output.txt" 2>/dev/null; } > "$OUT"
            say "The test didn't finish. Saved what happened to: $OUT"
            say "Send me that file."
        fi
        ;;
    t16)
        TDIR="$HERE/gpu-test"
        pick_wine
        say "Using: $CX_WINE"
        say "Running the skinning shader GPU check. It takes under a minute and"
        say "doesn't change anything. Please wait..."
        say ""
        EXE_WIN="Z:$(printf '%s' "$GAME_DIR/Spider-Man2.exe" | tr '/' '\\\\')"
        rm -f "$TDIR/skin-report.txt"
        # No window is needed: first try with Wine's Mac display driver off (it can crash
        # on new macOS versions), then normally. Wine's own messages go to a log file.
        for TRY in "winemac.drv=d" ""; do
            (cd "$TDIR" && WINEDEBUG=-all WINEDLLOVERRIDES="$TRY" "${RUN[@]}" skin_gpu_test.exe "$EXE_WIN" \
                golden-skin.bin skin-report.txt ..\\shader-fix\\sm2fix-skinning.dxil) > "$TDIR/wine-output.txt" 2>&1
            grep -q "^RESULT" "$TDIR/skin-report.txt" 2>/dev/null && break
            [ -n "$TRY" ] && say "First attempt didn't finish, trying again with the normal Wine setup..."
        done
        if grep -q "^RESULT" "$TDIR/skin-report.txt" 2>/dev/null; then
            OUT="$HOME/Desktop/skin-report-$(date +%Y%m%d-%H%M).txt"
            {
                echo "mac: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / macOS $(sw_vers -productVersion 2>/dev/null)"
                echo "bottle: $KIND / $NAME"
                cat "$TDIR/skin-report.txt"
            } > "$OUT"
            say ""
            grep "^RESULT" "$TDIR/skin-report.txt"
            say ""
            say "Saved: $OUT"
            say "Send me that file."
            open -R "$OUT" 2>/dev/null
        else
            OUT="$HOME/Desktop/skin-report-FAILED-$(date +%Y%m%d-%H%M).txt"
            { echo "wine: $CX_WINE"; cat "$TDIR/skin-report.txt" 2>/dev/null; echo "--- wine output (last 60 lines)"; \
              tail -n 60 "$TDIR/wine-output.txt" 2>/dev/null; } > "$OUT"
            say "The test didn't finish. Saved what happened to: $OUT"
            say "Send me that file."
        fi
        ;;
    t13)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin --skin-budget 65536 || pause_exit 1
        say ""
        line
        say "Test #13 installed. The game keeps a fixed-size list of character-animation"
        say "work for the GPU, and anything that doesn't fit is silently dropped (those"
        say "body parts stay in the T-pose). This test makes that list much bigger,"
        say "and logs how big the game made it on your Mac."
        say "  1. Start the game, load your save, look at Spider-Man and people nearby."
        say "  2. Quit from the in-game menu."
        say "  3. Run this again, pick 3, and send me the log (fixed or not)."
        line
        ;;
    t21)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin --trap-writer || pause_exit 1
        say ""
        line
        say "Test #21 installed. It sets a one-time tripwire on a character's bone data"
        say "and logs exactly which part of the game writes it, so I can find why that"
        say "code skips the T-posed pieces. The game may stutter for a moment; if it"
        say "crashes, just tell me."
        say "  1. Start the game, load your save, play ~2 minutes near people."
        say "  2. Quit from the in-game menu, run this again, pick 3, send the log."
        line
        ;;
    t20)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin || pause_exit 1
        say ""
        line
        say "Test #20 installed. It records every buffer the game copies back from the"
        say "GPU to the CPU, and checks whether any of them always come back empty."
        say "  1. Start the game, load your save, play ~2 minutes near people."
        say "  2. Quit from the in-game menu, run this again, pick 3, send the log."
        line
        ;;
    t19)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin --force-anim || pause_exit 1
        say ""
        line
        say "Test #19 installed. The logs showed the game never computes the bones for"
        say "some meshes (like that jacket), so they stay in the T-pose. This turns off"
        say "the game's animation culling for every character, ~20 s after it starts."
        say "  1. Start the game, load your save, wait ~30 s, look at people nearby."
        say "  2. Is the T-pose gone (or less)? A screenshot helps."
        say "  3. Quit from the in-game menu, run this again, pick 3, send the log."
        line
        ;;
    t18)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin || pause_exit 1
        say ""
        line
        say "Test #18 installed. The GPU check showed the skinning shader works on your"
        say "Mac, so the game must be feeding it something different. This logs exactly"
        say "which buffers it reads, and samples the bone data every ~10 seconds."
        say "  1. Start the game, load your save, play ~2 minutes near people"
        say "     (keep moving so the characters animate)."
        say "  2. Quit from the in-game menu, run this again, pick 3, send the log."
        line
        ;;
    t17)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin --replace-skinning || pause_exit 1
        say ""
        line
        say "Test #17 installed. The game's character-skinning shader is swapped for a"
        say "rewritten copy that computes exactly the same thing (checked bit for bit on"
        say "a reference GPU) but without the code pattern Apple's M1-M4 compiler seems"
        say "to get wrong."
        say "  1. Start the game, load your save, look at Spider-Man and people nearby."
        say "  2. Quit from the in-game menu, run this again, pick 3, send the log"
        say "     (and a screenshot), fixed or not."
        line
        ;;
    t15)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin --skip-skinning || pause_exit 1
        say ""
        line
        say "Test #15 installed. This is NOT a fix: it switches off the game's"
        say "character-skinning shader, to see whether its output is what you see."
        say "  1. Start the game, load your save, find Spider-Man and some people."
        say "  2. Look closely: do T-posed body parts (sleeves, Spider-Man) look the"
        say "     SAME as before, or do they vanish / stretch / turn into spikes?"
        say "     A screenshot is perfect."
        say "  3. Quit from the in-game menu, run this again, pick 3, send the log."
        line
        ;;
    t14)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --probe-skin || pause_exit 1
        say ""
        line
        say "Test #14 installed. Test #13 proved the game queues the animation work"
        say "and has room for it, yet the GPU never gets it. This one watches what"
        say "the game actually sends to the GPU at that moment."
        say "  1. Start the game, load your save, play ~2 minutes near people."
        say "  2. Quit from the in-game menu."
        say "  3. Run this again, pick 3, and send me the log."
        line
        ;;
    t11)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --trace-queries --fake-timestamps || pause_exit 1
        say ""
        line
        say "Test #11 installed. On your Mac, Apple's D3DMetal tells the game the GPU"
        say "clock runs at 60 Hz and every GPU timestamp is 0. This test gives the"
        say "game a real clock instead."
        say "  1. Start the game, load your save. Look at Spider-Man and the people"
        say "     around you: still T-posing? Switch suits once too."
        say "  2. Quit from the in-game menu."
        say "  3. Run this again, pick 3, and send me the log + what you saw."
        line
        ;;
    t10)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --trace-queries || pause_exit 1
        say ""
        line
        say "Test #10 installed. It records the questions the game asks the GPU"
        say "(\"was this visible?\", \"how long did that take?\") and the answers it"
        say "reads back, every 15 seconds. Speed should be normal."
        say "  1. Start the game, load your save, play 2-3 minutes near people."
        say "  2. Quit from the in-game menu."
        say "  3. Run this again, pick 3, and send me the log."
        line
        ;;
    t9)
        TDIR="$HERE/rosetta-test"
        pick_wine
        say "Using: $CX_WINE"
        say "Running the Rosetta instruction check. It takes 1-3 minutes and"
        say "doesn't touch the game. Please wait..."
        say ""
        (cd "$TDIR" && WINEDEBUG=-all "${RUN[@]}" rosetta_test.exe check golden.txt rosetta-report.txt)
        if [ -f "$TDIR/rosetta-report.txt" ]; then
            OUT="$HOME/Desktop/rosetta-report-$(date +%Y%m%d-%H%M).txt"
            {
                echo "mac: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / macOS $(sw_vers -productVersion 2>/dev/null)"
                echo "bottle: $KIND / $NAME"
                cat "$TDIR/rosetta-report.txt"
            } > "$OUT"
            say ""
            say "Saved: $OUT"
            say "Send me that file."
            open -R "$OUT" 2>/dev/null
        else
            say "The test didn't produce a report. Send me a screenshot of this window."
        fi
        ;;
    t8|t8b)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        if [ "$action" = t8 ]; then DELAY=10; else DELAY=40; fi
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --pso-delay "$DELAY" || pause_exit 1
        say ""
        line
        say "Test #8 installed: every time the game builds a graphics program,"
        say "it now waits ${DELAY} ms, imitating the slow first boot after a settings"
        say "change (the one time the suit came up fixed). Loading WILL take longer."
        say "  1. Start the game, load your save, check Spider-Man and NPCs."
        say "  2. Quit from the menu and START IT A SECOND TIME, check again."
        say "  3. Run this again, pick 3, send me the log + what you saw each time."
        line
        ;;
    t7|t7b)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        if [ "$action" = t7 ]; then CORES=8; else CORES=4; fi
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile --cpu-count "$CORES" || pause_exit 1
        say ""
        line
        say "Test #7 installed: the game now thinks your Mac has $CORES CPU cores."
        say "This changes how its loading threads overlap, which is where the"
        say "T-pose seems to come from. The animation tracker stays on."
        say "  1. Start the game, load your save, look at Spider-Man and NPCs."
        say "     Switch suits a couple of times."
        say "  2. Quit from the in-game menu. Then START IT ONE MORE TIME and"
        say "     check again (the problem is random, so two launches tell more)."
        say "  3. Run this again, pick 3, and send me the log + what you saw."
        line
        ;;
    t6)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode off --profile || pause_exit 1
        say ""
        line
        say "Test #6 installed. It counts how often the game runs its"
        say "character-animation (skinning) program while you play."
        say "Speed should be normal."
        say "  1. Start Spider-Man 2 and play 3-5 minutes. Include a moment"
        say "     where you switch suits, and note the time if Spider-Man"
        say "     starts animating properly."
        say "  2. Quit the game normally (from the menu)."
        say "  3. Run this again, pick 3, and send me the log it saves."
        line
        ;;
    t5|t5b)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        if [ "$action" = t5 ]; then MINMAX=linear; else MINMAX=point; fi
        bash "$INSTALLER" "${ARGS[@]}" --mode off --minmax "$MINMAX" --dump-graphics || pause_exit 1
        say ""
        line
        say "Test #5 installed. M5 Macs have a GPU feature older Macs lack"
        say "(\"min/max samplers\", used to decide what's hidden). This test"
        say "rewrites those samplers into ones M1-M4 support, and saves the"
        say "game's vertex shaders so I can find the animation code."
        say "  1. Start Spider-Man 2 and go where characters T-pose."
        say "     The first load may take a little longer (saving shaders)."
        say "  2. Quit the game normally."
        say "  3. Run this again, pick 3, and send me what it saves (2 files)."
        say "     Tell me: still T-posing? Anything look different/broken?"
        line
        ;;
    spoof|spoof_b)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        if [ "$action" = spoof ]; then SPOOF="sm65,no_int64,no_fp16"; else SPOOF="no_int64,no_fp16"; fi
        bash "$INSTALLER" "${ARGS[@]}" --mode off --spoof "$SPOOF" --dump-compute || pause_exit 1
        say ""
        line
        say "Test #4 installed. The game is told the GPU lacks a few features"
        say "($SPOOF), so it may take a different code path. Speed is normal."
        say "  1. Start Spider-Man 2 and go where characters T-pose."
        say "  2. Quit the game normally."
        say "  3. Run this again, pick 3, and send me what it saves (2 files)."
        say "     Tell me: still T-posing? Any crash or weird graphics?"
        line
        ;;
    1)
        ds_on
        rm -rf "$GAME_DIR/sm2fix-dumps"
        bash "$INSTALLER" "${ARGS[@]}" --mode max || pause_exit 1
        say ""
        line
        say "Deep test installed. The game will run SLOWER than usual. That's"
        say "expected: it makes the GPU finish each job before starting the next."
        say "  1. Start Spider-Man 2 and play for 2-3 minutes where characters"
        say "     T-pose (a cutscene is perfect). Note whether they still T-pose."
        say "  2. Quit the game normally."
        say "  3. Run this again and pick 3. Send me what it saves."
        line
        ;;
    2)
        if [ -f "$GAME_DIR/sm2fix.log" ]; then
            STAMP="$(date +%Y%m%d-%H%M)"
            OUT="$HOME/Desktop/sm2fix-log-$STAMP.txt"
            {
                echo "bottle: $KIND / $NAME"
                echo "mode: $(current_mode)"
                echo "mac: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / macOS $(sw_vers -productVersion 2>/dev/null)"
                echo "crossover: $(defaults read /Applications/CrossOver.app/Contents/Info CFBundleShortVersionString 2>/dev/null)"
                echo "game folder has dstorage: $(ls "$GAME_DIR" | grep -c '^dstorage.*\.dll$')"
                echo "----"
                cat "$GAME_DIR/sm2fix.log"
            } > "$OUT"
            say "Saved: $OUT"
            if [ -d "$GAME_DIR/sm2fix-capture" ] && [ -n "$(ls "$GAME_DIR/sm2fix-capture" 2>/dev/null)" ]; then
                # Uploads have a size limit: pack the recordings into zips of ~150 MB raw (they compress ~8x).
                ZDIR="$HOME/Desktop/sm2fix-capture-$STAMP"
                mkdir -p "$ZDIR"
                PART=1 SIZE=0 BATCH=()
                for F in "$GAME_DIR"/sm2fix-capture/*; do
                    FS=$(wc -c < "$F" | tr -d " ")
                    if [ ${#BATCH[@]} -gt 0 ] && [ $((SIZE + FS)) -gt 150000000 ]; then
                        (cd "$GAME_DIR/sm2fix-capture" && zip -q "$ZDIR/part$PART.zip" "${BATCH[@]}")
                        PART=$((PART + 1)) SIZE=0 BATCH=()
                    fi
                    BATCH+=("$(basename "$F")")
                    SIZE=$((SIZE + FS))
                done
                [ ${#BATCH[@]} -gt 0 ] && (cd "$GAME_DIR/sm2fix-capture" && zip -q "$ZDIR/part$PART.zip" "${BATCH[@]}")
                say "Saved: $ZDIR ($PART zip file(s))"
                say "Send me the log AND every zip in that folder."
            fi
            if [ -d "$GAME_DIR/sm2fix-dumps" ] && [ -n "$(ls "$GAME_DIR/sm2fix-dumps" 2>/dev/null)" ]; then
                ZIP="$HOME/Desktop/sm2fix-shaders-$STAMP.zip"
                (cd "$GAME_DIR" && zip -qr "$ZIP" sm2fix-dumps)
                say "Saved: $ZIP"
                say "Send me BOTH files."
            fi
            open -R "$OUT" 2>/dev/null
        else
            say "No results yet. Pick 1, play the game, quit it, then pick 3."
            say "If you already did, the game didn't load the test. Send me a"
            say "screenshot of this window."
        fi
        ;;
    3)
        bash "$INSTALLER" "${ARGS[@]}" --uninstall
        ds_on
        say "DirectStorage is ON. Everything is back to normal."
        ;;
    4)
        ds_on
        bash "$INSTALLER" "${ARGS[@]}" --mode all || pause_exit 1
        say ""
        say "Test #1 installed. Play, quit, then run this again and pick 3."
        ;;
    5)
        [ -n "$MODE_NOW" ] && bash "$INSTALLER" "${ARGS[@]}" --uninstall >/dev/null 2>&1
        ds_off
        say "Test #2: DirectStorage is OFF (other tests removed)."
        say "Play, then tell me if characters still T-pose."
        say "Pick 4 any time to undo."
        ;;
    6)
        bash "$INSTALLER" "${ARGS[@]}" --mode hash || pause_exit 1
        say ""
        say "Switched to the faster version."
        ;;
    *)
        say "Nothing changed."
        ;;
esac

pause_exit 0
