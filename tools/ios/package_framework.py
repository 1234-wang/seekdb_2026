#!/usr/bin/env python3
"""Package and inspect the Bazel-built iOS framework before exposing it to apps."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess


def output(*command):
    return subprocess.check_output(command, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "config", "header", "exports", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--simulator", action="store_true")
    args = parser.parse_args()
    binary = str(args.binary)
    platform = "IOSSIMULATOR" if args.simulator else "IOS"
    version = output("xcrun", "vtool", "-show-build", binary)
    if not re.search(r"platform\s+" + platform + r"\b", version):
        raise ValueError("Framework platform does not match " + platform)
    if not re.search(r"minos\s+18\.0(?:\.0)?\b", version):
        raise ValueError("Expected iOS 18.0 minimum deployment target")
    if output("lipo", "-archs", binary).strip() != "arm64" or "DYLIB" not in output("otool", "-hv", binary):
        raise ValueError("Expected an arm64 dynamic framework")
    identity = "@rpath/SeekDB.framework/SeekDB"
    if output("otool", "-D", binary).splitlines()[1].strip() != identity:
        raise ValueError("Unexpected framework install name")
    if "LC_RPATH" in output("otool", "-l", binary):
        raise ValueError("Framework must not retain build search paths")
    dependencies = [line.strip().split(" (", 1)[0] for line in output("otool", "-L", binary).splitlines()[1:]]
    if any(dep != identity and not dep.startswith(("/usr/lib/", "/System/Library/")) for dep in dependencies):
        raise ValueError("Non-system dynamic dependency: " + repr(dependencies))
    expected = set(args.exports.read_text().splitlines())
    actual = {line.split()[-1] for line in output("nm", "-gU", binary).splitlines() if line.strip()}
    if actual != expected:
        raise ValueError("Unexpected ABI exports: " + repr(actual ^ expected))
    revision = re.search(r'#define SEEKDB_IOS_BUILD_ID "([^"]+)"', args.config.read_text()).group(1)
    markers = output("strings", binary)
    if "SEEKDB_IOS_ARTIFACT_HOOK_MODE=disabled" not in markers or "SEEKDB_IOS_ARTIFACT_BUILD_ID=" + revision not in markers:
        raise ValueError("Missing framework build metadata")
    destination = args.output
    (destination / "Headers").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.header, destination / "Headers/seekdb.h")
    info = {
        "CFBundleExecutable": "SeekDB", "CFBundleIdentifier": "org.oceanbase.seekdb",
        "CFBundleName": "SeekDB", "CFBundlePackageType": "FMWK",
        "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
        "MinimumOSVersion": "18.0", "SeekDBSourceRevision": revision, "SeekDBTestHooks": "disabled",
    }
    (destination / "Info.plist").write_bytes(plistlib.dumps(info))
    digest = hashlib.sha256()
    with args.binary.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    manifest = {
        "source_revision": revision, "platform": platform, "architecture": "arm64",
        "deployment_target": "18.0", "test_hooks": "disabled", "binary_sha256": digest.hexdigest(),
        "dynamic_dependencies": dependencies, "exported_symbols": sorted(actual),
    }
    (destination / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Verified", platform, "arm64 framework:", args.binary)


if __name__ == "__main__":
    main()
