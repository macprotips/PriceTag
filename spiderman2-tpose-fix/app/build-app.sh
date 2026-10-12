#!/bin/bash
# Builds "Spider-Man 2 T-Pose Fix.app" (and a zip of it) into ./build.
#
#   ./build-app.sh                # native architecture
#   UNIVERSAL=1 ./build-app.sh    # Apple Silicon + Intel (needs full Xcode)
#   VERSION=1.2 ./build-app.sh
#
# The fix files come from the rest of the repo (prebuilt/d3d12.dll, shim/sm2fix.ini) and
# Microsoft's shader tools from ./build-dxc.sh (in .dxc). No game data goes in the app:
# it builds the fix's shaders from the user's own copy of the game.
set -euo pipefail
cd "$(dirname "$0")"
FIX="$(cd .. && pwd)"

VERSION="${VERSION:-1.0}"
BUILD_NUMBER="${BUILD_NUMBER:-1}"
ARCH_FLAGS=()
[[ "${UNIVERSAL:-0}" == "1" ]] && ARCH_FLAGS=(--arch arm64 --arch x86_64)

echo "==> Building release binary"
swift build -c release "${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"}"
BIN_DIR="$(swift build -c release "${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"}" --show-bin-path)"

APP="build/Spider-Man 2 T-Pose Fix.app"
echo "==> Assembling $APP"
rm -rf build
[[ -x .dxc/bin/dxc ]] || { echo "Run ./build-dxc.sh first (Microsoft's shader tools)." >&2; exit 1; }
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/payload" "$APP/Contents/Helpers" "$APP/Contents/Frameworks" \
    "$APP/Contents/Resources/Licenses"
cp "$BIN_DIR/SM2Fix" "$APP/Contents/MacOS/SM2Fix"
cp Resources/Info.plist "$APP/Contents/Info.plist"
plutil -replace CFBundleShortVersionString -string "$VERSION" "$APP/Contents/Info.plist"
plutil -replace CFBundleVersion -string "$BUILD_NUMBER" "$APP/Contents/Info.plist"
iconutil -c icns Resources/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"
cp "$FIX/prebuilt/d3d12.dll" "$FIX/shim/sm2fix.ini" "$APP/Contents/Resources/payload/"
(cd "$FIX/prebuilt" && shasum -a 256 -c d3d12.dll.sha256)
cp "$FIX/LICENSE" "$APP/Contents/Resources/Licenses/LICENSE.txt"

# Shader tools: executables in Helpers, libraries in Frameworks.
cp .dxc/bin/dxc .dxc/bin/dxa .dxc/bin/dxv "$APP/Contents/Helpers/"
cp .dxc/lib/*.dylib "$APP/Contents/Frameworks/"
cp .dxc/licenses/* "$APP/Contents/Resources/Licenses/"
for tool in dxc dxa dxv; do
    install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/Helpers/$tool" 2>/dev/null || true
done

echo "==> Signing"
SIGN=(--force --sign -)   # ad-hoc
[[ -n "${SIGN_IDENTITY:-}" ]] && SIGN=(--force --options runtime --timestamp --sign "$SIGN_IDENTITY")
for code in "$APP"/Contents/Frameworks/*.dylib "$APP"/Contents/Helpers/*; do
    codesign "${SIGN[@]}" "$code"
done
codesign "${SIGN[@]}" "$APP"
codesign --verify --strict "$APP"

ZIP="build/Spider-Man-2-T-Pose-Fix.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "==> Done: $APP ($ZIP)"
