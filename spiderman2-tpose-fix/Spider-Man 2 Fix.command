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

# Each match is stored as: kind|bottle name|bottle path|game folder
FOUND=()
scan() { # kind, bottles dir
    local kind="$1" root="$2" bottle exe
    [ -d "$root" ] || return
    for bottle in "$root"/*; do
        [ -d "$bottle/drive_c" ] || continue
        exe="$(find "$bottle/drive_c" -name 'Spider-Man2.exe' -not -path '*/windows/*' 2>/dev/null | head -n 1)"
        [ -n "$exe" ] && FOUND+=("$kind|$(basename "$bottle")|$bottle|$(dirname "$exe")")
    done
}
scan CrossOver "$HOME/Library/Application Support/CrossOver/Bottles"
scan Whisky "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Bottles"

if [ ${#FOUND[@]} -eq 0 ]; then
    say ""
    say "I couldn't find Spider-Man2.exe in any CrossOver or Whisky bottle."
    say "Drag the game's folder (the one containing Spider-Man2.exe) onto this"
    say "window, then press Return:"
    read -r -p "> " GAME_DIR
    GAME_DIR="${GAME_DIR%/}"; GAME_DIR="${GAME_DIR//\\ / }"; GAME_DIR="${GAME_DIR%\'}"; GAME_DIR="${GAME_DIR#\'}"
    if [ ! -f "$GAME_DIR/Spider-Man2.exe" ]; then
        say "That folder doesn't contain Spider-Man2.exe."
        pause_exit 1
    fi
    PREFIX="${GAME_DIR%%/drive_c/*}"
    FOUND=("Other|custom|$PREFIX|$GAME_DIR")
fi

CHOICE="${FOUND[0]}"
if [ ${#FOUND[@]} -gt 1 ]; then
    say ""
    say "Spider-Man 2 is in more than one bottle. Which one do you play from?"
    i=1
    for f in "${FOUND[@]}"; do
        IFS='|' read -r kind name _ _ <<< "$f"
        say "  $i) $kind bottle \"$name\""
        i=$((i + 1))
    done
    read -r -p "Type a number and press Return: " n
    case "$n" in ''|*[!0-9]*) n=1 ;; esac
    [ "$n" -ge 1 ] && [ "$n" -le ${#FOUND[@]} ] || n=1
    CHOICE="${FOUND[$((n - 1))]}"
fi
IFS='|' read -r KIND NAME PREFIX GAME_DIR <<< "$CHOICE"

ARGS=(--game-dir "$GAME_DIR")
if [ "$KIND" = CrossOver ]; then
    ARGS+=(--bottle "$NAME")
elif [ "$KIND" = Whisky ] && [ -x "$WHISKY_WINE" ]; then
    ARGS+=(--prefix "$PREFIX" --wine "$WHISKY_WINE")
else
    ARGS+=(--prefix "$PREFIX")
fi

current_mode() {
    if [ -f "$GAME_DIR/d3d12.dll" ] && grep -q sm2fix "$GAME_DIR/d3d12.dll" 2>/dev/null; then
        sed -n 's/^mode *= *//p' "$GAME_DIR/sm2fix.ini" 2>/dev/null | head -n 1
    fi
}

say ""
say "Found it: $KIND bottle \"$NAME\""
MODE_NOW="$(current_mode)"
if [ -n "$MODE_NOW" ]; then
    say "The fix is installed (mode: $MODE_NOW)."
else
    say "The fix is not installed yet."
fi

say ""
say "What do you want to do?"
say "  1) Install the fix (start here)"
say "  2) It worked! Switch to the faster version"
say "  3) Faster version brought the T-pose back: go back to the safe version"
say "  4) Save my log to the Desktop (to send for help)"
say "  5) Remove the fix completely"
say "  6) Quit"
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
    *)
        say "Nothing changed."
        ;;
esac

pause_exit 0
