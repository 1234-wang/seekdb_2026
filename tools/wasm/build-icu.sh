#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export SEEKDB_WASM_DEPS="${1:-${repo_root}/build_wasm_deps}"
if [[ ! -d "${SEEKDB_WASM_DEPS}/em-cache" ]]; then
  echo "Run tools/wasm/build-deps.sh first to prepare the Emscripten cache." >&2
  exit 1
fi
cd "${repo_root}"
./build.py build --config=wasm @seekdb_sdk_icu//:library
