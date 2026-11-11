#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source_dir=dist/deps/boringssl
revision=$(cat scripts/boringssl-revision.txt)
if [[ $(git -C "$source_dir" rev-parse HEAD 2>/dev/null || true) != "$revision" ]]; then
  mkdir -p "$source_dir"
  git -C "$source_dir" init -q
  git -C "$source_dir" fetch -q --depth 1 https://github.com/google/boringssl.git "$revision"
  git -C "$source_dir" checkout -q --detach FETCH_HEAD
fi
target=$1
shift
cmake -S "$source_dir" -B "dist/deps/$target" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DOPENSSL_NO_ASM=ON "$@"
cmake --build "dist/deps/$target" --target crypto --parallel 6
