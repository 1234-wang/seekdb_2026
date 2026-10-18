#!/usr/bin/env bash
# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
install_prefix="${1:-${repo_root}/build_wasm_deps}"
mkdir -p "${install_prefix}"
install_prefix="$(cd "${install_prefix}" && pwd)"
sdk_version="$(cat "${repo_root}/tools/wasm/emscripten-version")"
if ! emcc --version | head -n 1 | grep -Eq " ${sdk_version//./\\.}([ -]|$)"; then
  echo "Activate Emscripten ${sdk_version} before building Wasm dependencies." >&2
  exit 1
fi
# Download only the locked official archive and reject changed contents before
# extraction. Python 3.12+ data filtering prevents archive path traversal.
python3 - "${repo_root}/tools/wasm/dependencies.json" "${install_prefix}" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile

spec = json.loads(Path(sys.argv[1]).read_text())["openssl"]
root = Path(sys.argv[2]) / "sources"
root.mkdir(parents=True, exist_ok=True)
archive = root / (spec["source_directory"] + ".tar.gz")
if not archive.exists():
    download = archive.with_suffix(".download")
    subprocess.run(["curl", "--fail", "--location", "--connect-timeout", "20",
                    "--max-time", "180", spec["url"], "--output", str(download)], check=True)
    if hashlib.sha256(download.read_bytes()).hexdigest() != spec["sha256"]:
        raise SystemExit("OpenSSL source checksum mismatch")
    download.replace(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != spec["sha256"]:
    raise SystemExit("OpenSSL source checksum mismatch")
if not (root / spec["source_directory"]).exists():
    with tarfile.open(archive) as source:
        source.extractall(root, filter="data")
PY
openssl_source="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["openssl"]["source_directory"])' "${repo_root}/tools/wasm/dependencies.json")"
cd "${install_prefix}/sources/${openssl_source}"
CC=emcc AR=emar RANLIB=emranlib perl Configure linux-generic32 \
  no-asm no-shared no-tests no-dso no-engine no-async no-sock no-afalgeng no-ui-console \
  -pthread --prefix="${install_prefix}"
make -j4 build_libs
make install_dev
bash "${repo_root}/tools/wasm/build-libxml2.sh" "${install_prefix}"
bash "${repo_root}/tools/wasm/build-protobuf-c.sh" "${install_prefix}"

# Package the system libraries for seekdb's thread/exception ABI with this SDK.
# Bazel consumes this cache read-only; repository discovery never compiles.
export EM_CACHE="${install_prefix}/em-cache"
probe_dir="$(mktemp -d "${install_prefix}/sysroot.XXXXXX")"
trap 'rm -rf "${probe_dir}"' EXIT
printf 'int main() { return 0; }\n' > "${probe_dir}/main.cpp"
emcc "${probe_dir}/main.cpp" -Oz -pthread -fexceptions -msimd128 \
  -sUSE_ZLIB=1 -sWASMFS -sASSERTIONS=2 -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 \
  -o "${probe_dir}/main.mjs"

cd "${repo_root}"
SEEKDB_WASM_DEPS="${install_prefix}" ./build.py build --config=wasm //bazel/sdk_sources:source_sdk
