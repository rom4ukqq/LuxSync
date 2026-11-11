#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
crypto=dist/deps/mac-crypto
xcrun clang++ -std=c++17 -Wall -Wextra -Werror tests/pairing.cpp \
  "$crypto/pake.o" "$crypto/libcrypto.a" -o "$crypto/pairing-tests"
"$crypto/pairing-tests"
xcrun clang++ -dynamiclib "$crypto/pake.o" "$crypto/libcrypto.a" -o "$crypto/pairing-fixture.dylib"
python3 tests/simulation.py mac dist/macos/BrightSync.app/Contents/MacOS/BrightSync
python3 tests/pin_udp.py dist/macos/BrightSync.app/Contents/MacOS/BrightSync "$crypto/pairing-fixture.dylib"
