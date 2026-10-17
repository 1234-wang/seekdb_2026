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
python3 - "${repo_root}/tools/wasm/dependencies.json" "${install_prefix}" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile

spec = json.loads(Path(sys.argv[1]).read_text())["libxml2"]
prefix = Path(sys.argv[2])
root = prefix / "sources"
root.mkdir(parents=True, exist_ok=True)
archive = root / (spec["source_directory"] + ".tar.xz")
if not archive.exists():
    download = archive.with_suffix(".download")
    subprocess.run(["curl", "--fail", "--location", "--connect-timeout", "20",
                    "--max-time", "180", spec["url"], "--output", str(download)], check=True)
    if hashlib.sha256(download.read_bytes()).hexdigest() != spec["sha256"]:
        raise SystemExit("libxml2 archive checksum mismatch")
    download.replace(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != spec["sha256"]:
    raise SystemExit("libxml2 archive checksum mismatch")
source = root / spec["source_directory"]
with tarfile.open(archive) as files:
    files.extractall(root, filter="data")
build = prefix / "build" / "libxml2"
build.mkdir(parents=True, exist_ok=True)
subprocess.run(["emconfigure", str(source / "configure"),
                "--host=wasm32-unknown-emscripten", "--prefix=" + str(prefix),
                "--disable-shared", "--enable-static", "--with-threads",
                "--with-iconv", "--without-python",
                "--without-http", "--without-ftp", "--without-lzma",
                "--without-zlib", "--without-modules", "CFLAGS=-Oz -pthread"],
               cwd=build, check=True)
subprocess.run(["emmake", "make", "-j3", "libxml2.la"], cwd=build, check=True)
subprocess.run(["emmake", "make", "install-libLTLIBRARIES", "install-nodist_pkgconfigDATA"],
               cwd=build, check=True)
subprocess.run(["emmake", "make", "-C", "include", "install"], cwd=build, check=True)
PY
