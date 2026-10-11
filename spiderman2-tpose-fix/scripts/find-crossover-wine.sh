#!/bin/bash
# Prints the path of CrossOver's command-line `wine`, wherever CrossOver is installed.
# Exit status 1 if it can't be found. (bash 3.2 compatible)

if [ -n "$SM2FIX_CX_WINE" ]; then
    [ -x "$SM2FIX_CX_WINE" ] && { echo "$SM2FIX_CX_WINE"; exit 0; }
    exit 1
fi

wine_in_app() { # app bundle path
    local app="$1" w
    w="$app/Contents/SharedSupport/CrossOver/bin/wine"
    [ -x "$w" ] && { echo "$w"; return 0; }
    w="$(find "$app/Contents" -maxdepth 6 -path '*/bin/wine' -type f 2>/dev/null | head -n 1)"
    [ -n "$w" ] && [ -x "$w" ] && { echo "$w"; return 0; }
    return 1
}

# 1) the usual places, any app whose name starts with CrossOver
for app in /Applications/CrossOver*.app "$HOME"/Applications/CrossOver*.app /Applications/*/CrossOver*.app; do
    [ -d "$app" ] && wine_in_app "$app" && exit 0
done
# 2) Spotlight, by bundle id
if command -v mdfind >/dev/null 2>&1; then
    mdfind "kMDItemCFBundleIdentifier == 'com.codeweavers.CrossOver*'" 2>/dev/null | while IFS= read -r app; do
        wine_in_app "$app" && exit 0
    done | head -n 1 | grep . && exit 0
fi
# 3) a running CrossOver
app="$(ps -axo comm= 2>/dev/null | sed -n 's#^\(.*/CrossOver[^/]*\.app\)/.*#\1#p' | head -n 1)"
[ -n "$app" ] && wine_in_app "$app" && exit 0
exit 1
