#!/bin/bash
# Double-click me in Finder. Finds Spider-Man 2 in your CrossOver / Whisky bottles and
# installs, tunes or removes the T-pose fix. Safe to run as many times as you like.
# (Written for the bash 3.2 that ships with macOS.)

HERE="$(cd "$(dirname "$0")" && pwd)"
INSTALLER="$HERE/scripts/install-macos.sh"
CX_WINE="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
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
say "  1) Test #7: game sees 8 CPU cores  <-- do this one"
say "  2) Test #7b: game sees 4 CPU cores (after test #7)"
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
say "  10) Quit"
read -r -p "Type a number and press Return: " action
say ""

# Map the new menu numbers onto the handlers below.
case "$action" in
    1) action=t7 ;;
    2) action=t7b ;;
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

case "$action" in
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
