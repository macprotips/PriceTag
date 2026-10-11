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

say ""
say "Found it: $GAME_DIR"
[ "$KIND" != Other ] && say "Bottle:   $KIND \"$NAME\""
DS_OFF=0
[ -f "$GAME_DIR/dstorage.dll.sm2fix-off" ] || [ -f "$GAME_DIR/dstoragecore.dll.sm2fix-off" ] && DS_OFF=1
MODE_NOW="$(current_mode)"
if [ -n "$MODE_NOW" ]; then
    say "The fix is installed (mode: $MODE_NOW)."
else
    say "The fix is not installed yet."
fi
[ $DS_OFF -eq 1 ] && say "DirectStorage is turned OFF (test #2)."

say ""
say "What do you want to do?"
say "  1) Test #1: barrier fix (start here)"
say "  2) It worked! Switch to the faster version"
say "  3) Faster version brought the T-pose back: go back to the safe version"
say "  4) Save my log to the Desktop (to send for help)"
say "  5) Remove the barrier fix"
say "  6) Test #2: turn OFF DirectStorage (also removes test #1)"
say "  7) Turn DirectStorage back ON (undo test #2)"
say "  8) Quit"
read -r -p "Type a number and press Return: " action
say ""

case "$action" in
    1|3)
        bash "$INSTALLER" "${ARGS[@]}" --mode all || pause_exit 1
        say ""
        line
        say "Done! Now:"
        say "  1. Start Spider-Man 2 the usual way."
        say "  2. Go somewhere characters used to T-pose."
        say "  3. Fixed? Run this again and pick 2 for better performance."
        say "     Still broken? Run this again and pick 4, then send me the file."
        line
        ;;
    2)
        bash "$INSTALLER" "${ARGS[@]}" --mode hash || pause_exit 1
        say ""
        say "Switched to the faster version. If the T-pose comes back, run this"
        say "again and pick 3."
        ;;
    4)
        if [ -f "$GAME_DIR/sm2fix.log" ]; then
            OUT="$HOME/Desktop/sm2fix-log-$(date +%Y%m%d-%H%M).txt"
            {
                echo "bottle: $KIND / $NAME"
                echo "mode: $(current_mode)"
                echo "mac: $(sysctl -n machdep.cpu.brand_string 2>/dev/null) / macOS $(sw_vers -productVersion 2>/dev/null)"
                echo "game folder has dstorage: $(ls "$GAME_DIR" | grep -ci dstorage)"
                echo "----"
                cat "$GAME_DIR/sm2fix.log"
            } > "$OUT"
            say "Saved: $OUT"
            open -R "$OUT" 2>/dev/null
        else
            say "No log yet. Install the fix (option 1) and play the game first."
            say "If you already did, the game didn't load the fix. Send me a"
            say "screenshot of this window."
        fi
        ;;
    5)
        bash "$INSTALLER" "${ARGS[@]}" --uninstall
        ;;
    6)
        if [ -n "$MODE_NOW" ]; then
            bash "$INSTALLER" "${ARGS[@]}" --uninstall >/dev/null 2>&1
            say "Removed test #1 so this test is clean."
        fi
        n=0
        for f in dstorage.dll dstoragecore.dll; do
            if [ -f "$GAME_DIR/$f" ]; then
                mv "$GAME_DIR/$f" "$GAME_DIR/$f.sm2fix-off" && n=$((n + 1))
            fi
        done
        if [ $n -gt 0 ] || [ $DS_OFF -eq 1 ]; then
            say ""
            line
            say "DirectStorage is OFF. The game now unpacks its data on the CPU."
            say "Loading may be a little slower; that's expected."
            say "  1. Start Spider-Man 2 and go where characters used to T-pose."
            say "  2. Tell me: fixed or not?"
            say "  3. Either way you can undo this any time with option 7."
            line
        else
            say "No DirectStorage files found in the game folder, nothing to turn off."
        fi
        ;;
    7)
        n=0
        for f in dstorage.dll dstoragecore.dll; do
            if [ -f "$GAME_DIR/$f.sm2fix-off" ]; then
                mv "$GAME_DIR/$f.sm2fix-off" "$GAME_DIR/$f" && n=$((n + 1))
            fi
        done
        if [ $n -gt 0 ]; then say "DirectStorage is back ON."; else say "DirectStorage was already ON."; fi
        ;;
    *)
        say "Nothing changed."
        ;;
esac

pause_exit 0
