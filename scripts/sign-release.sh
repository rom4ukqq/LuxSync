#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 ]]; then
  printf 'Usage: %s <private-key-file> <zip-directory>\n' "$0" >&2
  exit 2
fi
key=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
output=$(cd "$2" && pwd)
cd "$(dirname "$0")/.."
go run ./updater sign "$(cat VERSION)" "$key" "$output"
