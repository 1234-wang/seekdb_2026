#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
"""Build the pinned Wasm dependencies and SDK source libraries."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile


def locked_source(prefix, spec, suffix, checksum_error, *, reuse=False):
    root = prefix / "sources"
    root.mkdir(parents=True, exist_ok=True)
    archive = root / (spec["source_directory"] + suffix)
    if not archive.exists():
        download = archive.with_suffix(".download")
        subprocess.run([
            "curl", "--fail", "--location", "--connect-timeout", "20",
            "--max-time", "180", spec["url"], "--output", str(download),
        ], check=True)
        if hashlib.sha256(download.read_bytes()).hexdigest() != spec["sha256"]:
            raise SystemExit(checksum_error)
        download.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec["sha256"]:
        raise SystemExit(checksum_error)
    source = root / spec["source_directory"]
    if not reuse or not source.exists():
        with tarfile.open(archive) as files:
            files.extractall(root, filter="data")
    return source


def build_openssl(prefix, spec):
    source = locked_source(prefix, spec, ".tar.gz", "OpenSSL source checksum mismatch", reuse=True)
    subprocess.run([
        "perl", "Configure", "linux-generic32",
        "no-asm", "no-shared", "no-tests", "no-dso", "no-engine", "no-async",
        "no-sock", "no-afalgeng", "no-ui-console", "-pthread", "--prefix=" + str(prefix),
    ], cwd=source, env=dict(os.environ, CC="emcc", AR="emar", RANLIB="emranlib"), check=True)
    subprocess.run(["make", "-j4", "build_libs"], cwd=source, check=True)
    subprocess.run(["make", "install_dev"], cwd=source, check=True)
    return source


def build_libxml2(prefix, spec):
    source = locked_source(prefix, spec, ".tar.xz", "libxml2 archive checksum mismatch")
    build = prefix / "build/libxml2"
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "emconfigure", str(source / "configure"),
        "--host=wasm32-unknown-emscripten", "--prefix=" + str(prefix),
        "--disable-shared", "--enable-static", "--with-threads",
        "--with-iconv", "--without-python", "--without-http", "--without-ftp",
        "--without-lzma", "--without-zlib", "--without-modules", "CFLAGS=-Oz -pthread",
    ], cwd=build, check=True)
    subprocess.run(["emmake", "make", "-j3", "libxml2.la"], cwd=build, check=True)
    subprocess.run([
        "emmake", "make", "install-libLTLIBRARIES", "install-nodist_pkgconfigDATA",
    ], cwd=build, check=True)
    subprocess.run(["emmake", "make", "-C", "include", "install"], cwd=build, check=True)


def build_protobuf_c(prefix, spec):
    source = locked_source(prefix, spec, ".tar.gz", "protobuf-c archive checksum mismatch")
    obj = source / "protobuf-c.o"
    subprocess.run([
        "emcc", "-Oz", "-pthread", "-c", str(source / "protobuf-c/protobuf-c.c"),
        "-o", str(obj),
    ], check=True)
    lib = prefix / "lib"
    lib.mkdir(exist_ok=True)
    subprocess.run(["emar", "rcs", str(lib / "libprotobuf-c.a"), str(obj)], check=True)
    headers = prefix / "include/protobuf-c"
    headers.mkdir(parents=True, exist_ok=True)
    (headers / "protobuf-c.h").write_bytes((source / "protobuf-c/protobuf-c.h").read_bytes())


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "install_prefix", nargs="?", help="install directory, defaults to deps/wasm in the repository",
    )
    args = parser.parse_args()
    prefix = Path(args.install_prefix or repo / "deps/wasm")
    prefix.mkdir(parents=True, exist_ok=True)
    prefix = prefix.resolve()
    sdk_version = (repo / "tools/wasm/emscripten-version").read_text().strip()
    version_error = f"Activate Emscripten {sdk_version} before building Wasm dependencies."
    try:
        compiler = subprocess.run(["emcc", "--version"], stdout=subprocess.PIPE, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit(version_error)
    if not re.search(r" " + re.escape(sdk_version) + r"([ -]|$)", compiler.stdout.partition("\n")[0]):
        raise SystemExit(version_error)

    dependencies = json.loads((repo / "tools/wasm/dependencies.json").read_text())
    source = build_openssl(prefix, dependencies["openssl"])
    os.chdir(source)
    build_libxml2(prefix, dependencies["libxml2"])
    build_protobuf_c(prefix, dependencies["protobuf_c"])

    # Bazel consumes this prepared thread/exception ABI cache read-only.
    env = dict(os.environ, EM_CACHE=str(prefix / "em-cache"))
    with tempfile.TemporaryDirectory(prefix="sysroot.", dir=prefix) as temporary:
        probe = Path(temporary)
        (probe / "main.cpp").write_text("int main() { return 0; }\n")
        subprocess.run([
            "emcc", str(probe / "main.cpp"), "-Oz", "-pthread", "-fexceptions", "-msimd128",
            "-sUSE_ZLIB=1", "-sWASMFS", "-sASSERTIONS=2", "-sPROXY_TO_PTHREAD=1", "-sEXIT_RUNTIME=1",
            "-o", str(probe / "main.mjs"),
        ], env=env, check=True)
        subprocess.run([
            "./build.py", "build", "--config=wasm", "//bazel/sdk_sources:source_sdk",
        ], cwd=repo, env=dict(env, SEEKDB_WASM_DEPS=str(prefix)), check=True)


if __name__ == "__main__":
    main()
