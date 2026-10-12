#!/bin/bash
# Builds "Spider-Man 2 T-Pose Fix.app" (and a zip of it) into ./build.
#
#   ./build-app.sh                # native architecture
#   UNIVERSAL=1 ./build-app.sh    # Apple Silicon + Intel (needs full Xcode)
#   VERSION=1.2 ./build-app.sh
#
# The fix files come from the rest of the repo: prebuilt/d3d12.dll, shim/sm2fix.ini and
# shader-fix/sm2fix-pool-*.dxil.
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
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/payload"
cp "$BIN_DIR/SM2Fix" "$APP/Contents/MacOS/SM2Fix"
cp Resources/Info.plist "$APP/Contents/Info.plist"
plutil -replace CFBundleShortVersionString -string "$VERSION" "$APP/Contents/Info.plist"
plutil -replace CFBundleVersion -string "$BUILD_NUMBER" "$APP/Contents/Info.plist"
iconutil -c icns Resources/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"
cp "$FIX/prebuilt/d3d12.dll" "$FIX/shim/sm2fix.ini" "$FIX"/shader-fix/sm2fix-pool-*.dxil "$APP/Contents/Resources/payload/"
(cd "$FIX/prebuilt" && shasum -a 256 -c d3d12.dll.sha256)

echo "==> Signing"
if [[ -n "${SIGN_IDENTITY:-}" ]]; then
    codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP"
else
    codesign --force --sign - "$APP"   # ad-hoc
fi
codesign --verify --strict "$APP"

ZIP="build/Spider-Man-2-T-Pose-Fix.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "==> Done: $APP ($ZIP)"
