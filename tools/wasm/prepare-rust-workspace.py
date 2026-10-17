#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
"""Stage the Wasm-only Cargo dependency patch without editing Cargo's cache."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]).resolve()
workspace = build / "workspace"
patch = json.loads((repo / "tools/wasm/dependencies.json").read_text())["spdlog_rs_emscripten"]
dependency = build / "patched-crates/spdlog-rs"


def write_changed(path, content):
    if not path.exists() or path.read_bytes() != content:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)


staged_sources = set()
extensions = (".rs", ".h", ".py", ".toml", ".yaml", ".yml", ".json", ".lock")
for directory, subdirectories, files in os.walk(repo / "rust"):
    subdirectories[:] = [name for name in subdirectories if name not in ("target", ".git", ".cargo")]
    for name in files:
        source = Path(directory) / name
        if source.suffix in extensions:
            destination = workspace / source.relative_to(repo / "rust")
            staged_sources.add(destination)
            write_changed(destination, source.read_bytes())

# A removed Rust module must not survive in the private workspace and mask an
# otherwise missing source on the next incremental build.
for directory, subdirectories, files in os.walk(workspace):
    subdirectories[:] = [name for name in subdirectories if name != ".cargo"]
    for name in files:
        path = Path(directory) / name
        if path.suffix in extensions and path not in staged_sources:
            path.unlink()

patched_file = dependency / patch["path"]
if not patched_file.exists():
    metadata = json.loads(subprocess.check_output([
        "cargo", "metadata", "--locked", "--format-version", "1", "--manifest-path", str(workspace / "Cargo.toml"),
    ], cwd=workspace, text=True))
    package = next(package for package in metadata["packages"]
                   if package["name"] == "spdlog-rs" and package["version"] == patch["version"])
    original = Path(package["manifest_path"]).parent
    data = (original / patch["path"]).read_bytes()
    if hashlib.sha256(data).hexdigest() != patch["input_sha256"]:
        raise SystemExit("spdlog-rs Wasm patch input changed")
    for before, after in patch["replacements"]:
        if data.count(before.encode()) != 1:
            raise SystemExit("spdlog-rs Wasm patch context changed")
        data = data.replace(before.encode(), after.encode())
    if hashlib.sha256(data).hexdigest() != patch["output_sha256"]:
        raise SystemExit("spdlog-rs Wasm patch output changed")
    shutil.copytree(original, dependency)
    patched_file.write_bytes(data)
elif hashlib.sha256(patched_file.read_bytes()).hexdigest() != patch["output_sha256"]:
    raise SystemExit("staged spdlog-rs Wasm patch changed")

write_changed(workspace / ".cargo/config.toml", (
    '[patch.crates-io]\nspdlog-rs = { path = ' + json.dumps(str(dependency)) + ' }\n'
).encode())

# The path patch changes this private workspace's lock entry. Resolve that
# change offline now; the subsequent compilation still runs with --locked.
subprocess.run([
    "cargo", "metadata", "--offline", "--format-version", "1",
    "--manifest-path", str(workspace / "Cargo.toml"),
], cwd=workspace, check=True, stdout=subprocess.DEVNULL)
