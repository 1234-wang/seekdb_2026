#!/usr/bin/env python3
"""Install the pinned Bazel and keep build outputs and caches in known locations."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from typing import List, Optional


class BuildError(RuntimeError):
    """The seekdb build environment could not be prepared."""


def _host_platform() -> str:
    system = {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}.get(
        platform.system()
    )
    machine = platform.machine().lower()
    architecture = {
        "x86_64": "x86_64", "amd64": "x86_64",
        "aarch64": "arm64", "arm64": "arm64",
    }.get(machine)
    if not system or not architecture:
        raise BuildError("unsupported Bazel host: %s %s" % (platform.system(), machine))
    return "%s-%s%s" % (system, architecture, ".exe" if system == "windows" else "")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _download_binary(url: str, destination: Path, checksum: str) -> Path:
    """Verify before atomic installation; use the environment's proxy settings."""
    if destination.is_file() and _sha256_file(destination) == checksum:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    opener = urllib.request.build_opener()
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=destination.name + ".", dir=str(destination.parent)
    )
    temporary = Path(temporary_name)
    display_url = urllib.parse.urlsplit(url)._replace(query="", fragment="").geturl()
    try:
        print("Downloading Bazel: %s" % display_url, file=sys.stderr, flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "seekdb-bazel-bootstrap/1"})
        with os.fdopen(descriptor, "wb") as output:
            with opener.open(request, timeout=30) as response:
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    output.write(chunk)
        if _sha256_file(temporary) != checksum:
            raise BuildError("SHA256 mismatch for %s" % display_url)
        temporary.chmod(0o755)
        os.replace(str(temporary), str(destination))
        return destination
    except (OSError, ValueError) as error:
        raise BuildError("cannot download Bazel from %s: %s" % (display_url, error)) from error
    finally:
        if temporary.exists():
            temporary.unlink()


def _ensure_bazel(root: Path, cache_directory: Path, version: str) -> Path:
    manifest_path = root / "bazel" / "binaries.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        host = _host_platform()
        binary = manifest["platforms"][host]
        checksum = binary["sha256"]
        if manifest["version"] != version or not re.fullmatch(r"[0-9a-f]{64}", checksum):
            raise ValueError("version/checksum does not match .bazelversion")
        url = binary["url"]
        if not isinstance(url, str):
            raise ValueError("url must be an HTTP or HTTPS URL")
        source = urllib.parse.urlsplit(url)
        if source.scheme not in ("http", "https") or not source.hostname or source.fragment:
            raise ValueError("url must be an HTTP or HTTPS URL without a fragment")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise BuildError("invalid Bazel binary manifest %s: %s" % (manifest_path, error))
    filename = "bazel-%s-%s" % (version, host)
    destination = cache_directory / "binaries" / version / filename
    return _download_binary(url, destination, checksum)


def _cache_directory() -> Path:
    if os.name == "nt":
        return Path(os.environ["LOCALAPPDATA"]) / "bazel"
    if sys.platform == "darwin":
        return Path.home() / "Library/Caches/bazel"
    return Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "bazel"


def _quote(value: str) -> str:
    return '"' + value.replace('"', '\\"') + '"'


def _initialize(root: Path, arguments: List[str]) -> Optional[int]:
    if not arguments or arguments[0] != "init":
        return None
    parser = argparse.ArgumentParser(prog="build.py init")
    target_platform = parser.add_mutually_exclusive_group()
    target_platform.add_argument("--android", action="store_true")
    target_platform.add_argument("--ios", action="store_true")
    target_platform.add_argument("--wasm", action="store_true")
    parser.add_argument("--simulator", action="store_true")
    options = parser.parse_args(arguments[1:])
    if options.simulator and not options.ios:
        parser.error("--simulator requires --ios")
    if os.name == "nt":
        if options.android or options.ios or options.wasm:
            parser.error("Android/iOS/Wasm dependency initialization requires a Unix host")
        return subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
             str(root / "deps/init/dep_create.ps1")],
            cwd=str(root),
        ).returncode

    initializer = root / "deps/init/dep_create.sh"
    environment = os.environ.copy()
    environment["ANDROID_BUILD"] = "false"
    environment["WORKSPACE_DEPS_3RD"] = str(root / "deps/3rd")
    if options.ios:
        sdk = "iphonesimulator" if options.simulator else "iphoneos"
        if sys.platform != "darwin" or subprocess.run(
            ["xcrun", "--sdk", sdk, "--show-sdk-path"], env=environment,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode:
            raise BuildError("iOS SDK unavailable; install Xcode and set DEVELOPER_DIR to its Contents/Developer directory")
    status = subprocess.run(
        ["bash", initializer.name],
        cwd=str(initializer.parent),
        env=environment,
    ).returncode
    if status:
        return status
    if options.wasm:
        return subprocess.run(
            [sys.executable, str(root / "deps/init/wasm/build.py")], cwd=root, env=environment,
        ).returncode
    if options.android:
        environment["ANDROID_BUILD"] = "true"
        environment["WORKSPACE_DEPS_3RD"] = str(root / "deps/android")
        status = subprocess.run(["bash", initializer.name], cwd=str(initializer.parent), env=environment).returncode
    if status or not (options.ios or options.android):
        return status
    target = "aarch64-linux-android" if options.android else (
        "aarch64-apple-ios-sim" if options.simulator else "aarch64-apple-ios")
    status = subprocess.run(
        ["rustup", "target", "add", target], cwd=str(root / "rust"), env=environment,
    ).returncode
    if status or options.android:
        return status
    status = subprocess.run(
        [sys.executable, str(root / "deps/init/ios/build.py"), "--reuse", "--jobs", str(os.cpu_count() or 1)]
        + (["--simulator"] if options.simulator else []),
        cwd=str(root), env=environment,
    ).returncode
    if status:
        return status
    return main([
        "build", "--config=" + ("ios-simulator" if options.simulator else "ios"),
        "//bazel/sdk_sources:source_sdk",
    ])


def main(arguments: List[str]) -> int:
    root = Path(__file__).resolve().parent
    status = _initialize(root, arguments)
    if status is not None:
        return status

    # Parse launcher options only before the Bazel command. Everything after
    # the command (including `run -- ...`) belongs to Bazel or the program.
    parser = argparse.ArgumentParser(
        description=__doc__,
        usage="%(prog)s [--build-dir PATH] [--launcher-cache-dir PATH] <bazel command> [args]",
        epilog="Prepare project dependencies with: ./build.py init [--android | --ios [--simulator] | --wasm]",
        allow_abbrev=False,
    )
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--launcher-cache-dir", type=Path, default=_cache_directory())
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    options, startup_options = parser.parse_known_args(arguments)
    build_directory = (root / options.build_dir.expanduser()).resolve()
    if build_directory == root:
        raise BuildError("--build-dir must not name the workspace root")
    cache = options.launcher_cache_dir.expanduser().resolve()
    version = (root / ".bazelversion").read_text().strip()
    bazel = _ensure_bazel(root, cache, version)
    build_directory.mkdir(parents=True, exist_ok=True)

    # Only machine-specific paths belong here. Build policy lives in .bazelrc;
    # ordinary Bazel flags on the command line override these defaults.
    status_command = (
        "python bazel/build_metadata.py status"
        if os.name == "nt" else shlex.join([
            sys.executable,
            str(root / "bazel/build_metadata.py"),
            "status",
        ])
    )
    lines = [
        "common --downloader_config=" + _quote((root / "bazel/downloader.cfg").as_posix()),
        "build --disk_cache=" + _quote((_cache_directory() / "disk-cache").as_posix()),
        "build --symlink_prefix=" + _quote(build_directory.as_posix() + "/"),
        "build --workspace_status_command=" + _quote(status_command),
    ]
    if os.name == "nt":
        lines.append("build --repo_env=" + _quote(
            "BAZEL_LLVM=" + (root / "deps/3rd/tools/llvm18").resolve().as_posix()
        ))
    descriptor, name = tempfile.mkstemp(prefix="runtime.", suffix=".bazelrc", dir=cache)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write("\n".join(lines) + "\n")
        return subprocess.run([
            str(bazel), "--output_base=" + str(build_directory / "output-base"),
            "--bazelrc=" + name,
        ] + startup_options + options.arguments, cwd=root).returncode
    finally:
        Path(name).unlink()


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (BuildError, OSError) as error:
        print("ERROR: %s" % error, file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
