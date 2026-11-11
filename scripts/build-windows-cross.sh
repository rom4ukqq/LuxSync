#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dist/windows
version=$(cat VERSION)
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -trimpath \
  -ldflags "-s -w -H=windowsgui -X main.currentVersion=$version" \
  -o dist/windows/BrightSyncUpdater.exe ./updater
cp updater/Go-LICENSE.txt dist/windows/Go-LICENSE.txt
scripts/build-crypto.sh win-cross-crypto -DCMAKE_TOOLCHAIN_FILE="$PWD/scripts/mingw-x64.cmake"
x86_64-w64-mingw32-windres -i windows/brightsync/resources.rc -o dist/deps/win-cross-crypto/resources.o
x86_64-w64-mingw32-gcc -Os -I dist/deps/boringssl/include \
  -c protocol/pake.c -o dist/deps/win-cross-crypto/pake.o
"${MINGW_CXX:-x86_64-w64-mingw32-g++}" -std=c++17 -Wall -Wextra -Os -flto -static \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -municode -mwindows -s \
  windows/brightsync/main.cpp dist/deps/win-cross-crypto/pake.o dist/deps/win-cross-crypto/resources.o \
  dist/deps/win-cross-crypto/libcrypto.a -o dist/windows/BrightSync.exe \
  -ldxva2 -lbcrypt -lcrypt32 -lws2_32 -lshell32 -lole32 -ladvapi32 -luuid -lgdi32 -lwtsapi32
cp LICENSE dist/windows/LICENSE.txt
cp dist/deps/boringssl/LICENSE dist/windows/BoringSSL-LICENSE.txt
rm -f dist/BrightSync-Windows-x64.zip
(cd dist/windows && zip -q ../BrightSync-Windows-x64.zip BrightSync.exe BrightSyncUpdater.exe LICENSE.txt BoringSSL-LICENSE.txt Go-LICENSE.txt)
