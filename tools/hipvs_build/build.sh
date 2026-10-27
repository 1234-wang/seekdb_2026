#!/usr/bin/env bash
# Small GPU-only helper. Requires existing matching hipVS/ROCm; no installation or VSAG link.
set -Eeuo pipefail
[[ $# == 1 ]] || { echo 'usage: build.sh NEW_ABSOLUTE_HELPER_BINARY' >&2; exit 2; }
SOURCE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/seekdb_hipvs_build.cpp
OUTPUT=$1
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
[[ "$OUTPUT" == /* && ! -e "$OUTPUT" && ! -L "$OUTPUT" && -d "$(dirname -- "$OUTPUT")" ]] || { echo 'Require a new absolute output with an existing parent.' >&2; exit 2; }
[[ -x "$HIPCC" && -f "$HIPVS_ROOT/include/cuvs/neighbors/hnsw.h" && -f "$HIPVS_ROOT/lib/libcuvs_c.so" ]] || { echo 'Existing matching hipVS/ROCm SDK is required.' >&2; exit 2; }
TEMP=$(mktemp "$(dirname -- "$OUTPUT")/.hipvs-helper-build.XXXXXX")
trap 'rm -f -- "$TEMP" "$TEMP.ldd"' EXIT
GPU_LIBRARY_PATH="$HIPVS_ROOT/lib:$ROCM_ROOT/lib"
env -u LD_PRELOAD LD_LIBRARY_PATH="$GPU_LIBRARY_PATH" "$HIPCC" -std=c++17 -O2 -D__HIP_PLATFORM_AMD__ \
  -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include" "$SOURCE" \
  -L"$HIPVS_ROOT/lib" -L"$ROCM_ROOT/lib" -Wl,-rpath,"$GPU_LIBRARY_PATH" \
  -lcuvs_c -lamdhip64 -pthread -ldl -rdynamic -o "$TEMP"
env -u LD_PRELOAD LD_LIBRARY_PATH="$GPU_LIBRARY_PATH" ldd "$TEMP" > "$TEMP.ldd"
if grep -E -q 'not found|libvsag|/vsag-sdk/' "$TEMP.ldd"; then cat "$TEMP.ldd" >&2; exit 2; fi
chmod 0755 -- "$TEMP"
ln -- "$TEMP" "$OUTPUT"
sha256sum -- "$SOURCE" "$OUTPUT"
cat -- "$TEMP.ldd"
echo 'Compiled only. Runtime still requires a new owned directory and an isolated child process.'
