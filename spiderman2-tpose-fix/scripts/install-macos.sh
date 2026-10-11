#!/usr/bin/env bash
# Installs (or removes) the sm2fix d3d12.dll shim into a CrossOver / Whisky / GPTK
# bottle that has Marvel's Spider-Man 2 installed.
#
#   ./scripts/install-macos.sh --bottle "Steam"                 # CrossOver bottle name
#   ./scripts/install-macos.sh --prefix ~/path/to/wineprefix    # Whisky / GPTK prefix
#   ./scripts/install-macos.sh --bottle "Steam" --mode all      # diagnostic mode
#   ./scripts/install-macos.sh --bottle "Steam" --uninstall
#
# Optional: --game-dir "/path/to/Marvel's Spider-Man 2"  --wine /path/to/wine
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
BOTTLE="" PREFIX="" GAME_DIR="" WINE="" MODE="hash" UNINSTALL=0 SPOOF="" DUMP_COMPUTE=0 DUMP_GRAPHICS=0 MINMAX=keep PROFILE=0 CPU_COUNT=0 PSO_DELAY=0 TRACE_QUERIES=0 FAKE_TS=0 PROBE_SKIN=0 SKIN_BUDGET=0 SKIP_SKINNING=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bottle) BOTTLE="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --game-dir) GAME_DIR="$2"; shift 2 ;;
        --wine) WINE="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        --spoof) SPOOF="$2"; shift 2 ;;
        --dump-compute) DUMP_COMPUTE=1; shift ;;
        --dump-graphics) DUMP_GRAPHICS=1; shift ;;
        --minmax) MINMAX="$2"; shift 2 ;;
        --profile) PROFILE=1; shift ;;
        --cpu-count) CPU_COUNT="$2"; shift 2 ;;
        --pso-delay) PSO_DELAY="$2"; shift 2 ;;
        --trace-queries) TRACE_QUERIES=1; shift ;;
        --fake-timestamps) FAKE_TS=1; shift ;;
        --probe-skin) PROBE_SKIN=1; shift ;;
        --skin-budget) SKIN_BUDGET="$2"; shift 2 ;;
        --skip-skinning) SKIP_SKINNING=1; shift ;;
        -h|--help) sed -n 2,12p "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

CX_WINE="$(bash "$HERE/scripts/find-crossover-wine.sh" 2>/dev/null || true)"

if [[ -n "$BOTTLE" ]]; then
    PREFIX="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
    [[ -z "$WINE" && -x "$CX_WINE" ]] && WINE="$CX_WINE"
fi
# A game in an external Steam library only needs --game-dir; the bottle is then just
# where the DLL override goes (we print manual steps if we don't know it).
if [[ ( -z "$PREFIX" || ! -d "$PREFIX/drive_c" ) && -z "$GAME_DIR" ]]; then
    echo "Can't find a bottle at '$PREFIX'. Pass --bottle NAME (CrossOver) or --prefix PATH." >&2
    exit 1
fi

if [[ -z "$GAME_DIR" ]]; then
    exe="$(find "$PREFIX/drive_c" -name 'Spider-Man2.exe' -not -path '*/windows/*' 2>/dev/null | head -n 1 || true)"
    if [[ -z "$exe" ]]; then
        echo "Spider-Man2.exe not found in $PREFIX. Pass --game-dir." >&2
        exit 1
    fi
    GAME_DIR="$(dirname "$exe")"
fi
echo "Game folder: $GAME_DIR"

# Runs `wine reg ...` inside the bottle so Wine loads our d3d12.dll from the game folder
# before the translation layer's. Scoped to Spider-Man2.exe only.
REG_KEY='HKCU\Software\Wine\AppDefaults\Spider-Man2.exe\DllOverrides'
wine_reg() {
    [[ -d "$PREFIX/drive_c" ]] || return 1
    if [[ -n "$BOTTLE" && -n "$WINE" && "$WINE" == "$CX_WINE" ]]; then
        "$WINE" --bottle "$BOTTLE" reg "$@"
    elif [[ -n "$WINE" ]]; then
        WINEPREFIX="$PREFIX" "$WINE" reg "$@"
    else
        return 1
    fi
}

if [[ $UNINSTALL -eq 1 ]]; then
    rm -f "$GAME_DIR/d3d12.dll" "$GAME_DIR/sm2fix.ini" "$GAME_DIR/sm2fix.log"
    rm -rf "$GAME_DIR/sm2fix-dumps"
    [[ -f "$GAME_DIR/d3d12.dll.sm2fix-backup" ]] && mv "$GAME_DIR/d3d12.dll.sm2fix-backup" "$GAME_DIR/d3d12.dll"
    wine_reg delete "$REG_KEY" /v d3d12 /f >/dev/null 2>&1 || true
    echo "Removed sm2fix."
    exit 0
fi

DLL="$HERE/prebuilt/d3d12.dll"
[[ -f "$HERE/build/d3d12.dll" ]] && DLL="$HERE/build/d3d12.dll"
if [[ ! -f "$DLL" ]]; then
    echo "No d3d12.dll found. Run ./build.sh first." >&2
    exit 1
fi

if [[ -f "$GAME_DIR/d3d12.dll" ]] && ! grep -q "sm2fix" "$GAME_DIR/d3d12.dll" 2>/dev/null; then
    echo "Backing up the game's existing d3d12.dll"
    mv "$GAME_DIR/d3d12.dll" "$GAME_DIR/d3d12.dll.sm2fix-backup"
fi
if [[ ! -w "$GAME_DIR" ]]; then
    echo "Can't write to $GAME_DIR." >&2
    echo "If the game is on an external drive formatted as NTFS, macOS can only read it." >&2
    exit 1
fi
PREV_LOADED=0
grep -q "D3D12CreateDevice ok" "$GAME_DIR/sm2fix.log" 2>/dev/null && PREV_LOADED=1
cp "$DLL" "$GAME_DIR/d3d12.dll"
rm -f "$GAME_DIR/sm2fix.log"   # each test starts with a fresh log
sed -e "s/^mode = .*/mode = $MODE/" -e "s/^spoof =.*/spoof = $SPOOF/" \
    -e "s/^dump_compute = .*/dump_compute = $DUMP_COMPUTE/" -e "s/^dump_graphics = .*/dump_graphics = $DUMP_GRAPHICS/" \
    -e "s/^minmax = .*/minmax = $MINMAX/" -e "s/^profile = .*/profile = $PROFILE/" \
    -e "s/^cpu_count = .*/cpu_count = $CPU_COUNT/" -e "s/^pso_delay_ms = .*/pso_delay_ms = $PSO_DELAY/" \
    -e "s/^trace_queries = .*/trace_queries = $TRACE_QUERIES/" -e "s/^fake_timestamps = .*/fake_timestamps = $FAKE_TS/" \
    -e "s/^probe_skin = .*/probe_skin = $PROBE_SKIN/" -e "s/^skin_budget = .*/skin_budget = $SKIN_BUDGET/" \
    -e "s/^skip_skinning = .*/skip_skinning = $SKIP_SKINNING/" \
    "$HERE/shim/sm2fix.ini" > "$GAME_DIR/sm2fix.ini"

if grep -qi 'AppDefaults\\\\Spider-Man2.exe\\\\DllOverrides' "$PREFIX/user.reg" 2>/dev/null \
        && grep -qi '"d3d12"="native,builtin"' "$PREFIX/user.reg" 2>/dev/null; then
    echo "DLL override already set."
elif wine_reg add "$REG_KEY" /v d3d12 /t REG_SZ /d "native,builtin" /f >/dev/null 2>&1; then
    echo "Set per-game DLL override d3d12=native,builtin."
elif [[ $PREV_LOADED -eq 1 ]]; then
    echo "(DLL override not set, but a previous run shows the game loads sm2fix anyway: fine.)"
else
    cat <<EOF
Could not set the DLL override automatically. Do it once by hand:
  CrossOver: select the bottle > Wine Configuration > Libraries tab >
             type d3d12 > Add > Edit > "Native then Builtin".
  Whisky:    Bottle > Wine Configuration > Libraries, same steps.
EOF
fi

echo "Installed sm2fix (mode=$MODE${SPOOF:+, spoof=$SPOOF}, minmax=$MINMAX). Launch the game, then check:"
echo "  $GAME_DIR/sm2fix.log"
