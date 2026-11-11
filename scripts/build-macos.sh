#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
src=macos/BrightSync
app=dist/macos/BrightSync.app
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$src/Info.plist" "$app/Contents/Info.plist"
cp LICENSE "$app/Contents/Resources/LICENSE.txt"
scripts/build-crypto.sh mac-crypto -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cp dist/deps/boringssl/LICENSE "$app/Contents/Resources/BoringSSL-LICENSE.txt"
cp assets/BrightSync.icns "$app/Contents/Resources/"
xcrun clang -target arm64-apple-macos13.0 -Os -I dist/deps/boringssl/include \
  -c protocol/pake.c -o dist/deps/mac-crypto/pake.o
xcrun swiftc -target arm64-apple-macos13.0 -Osize -whole-module-optimization \
  -import-objc-header protocol/pake.h "$src/"*.swift dist/deps/mac-crypto/pake.o \
  dist/deps/mac-crypto/libcrypto.a -Xlinker -dead_strip -o "$app/Contents/MacOS/BrightSync"
strip -x "$app/Contents/MacOS/BrightSync"
if [[ -n "${MACOS_SIGN_IDENTITY:-}" && "$MACOS_SIGN_IDENTITY" != "-" ]]; then
  codesign --force --options runtime --timestamp --sign "$MACOS_SIGN_IDENTITY" "$app"
else
  codesign --force --sign - "$app"
fi
rm -f dist/BrightSync-macOS-arm64.zip
ditto -c -k --norsrc --keepParent "$app" dist/BrightSync-macOS-arm64.zip
