#!/usr/bin/env python3
"""Install the pinned Bazel and keep build outputs and caches in known locations."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import List, Optional

from tools.bazel_bootstrap import BootstrapError, ensure_bazel


class LauncherError(RuntimeError):
    """Invalid seekdb initialization or output directory."""


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
    parser = argparse.ArgumentParser(prog="bazel.py init")
    platform = parser.add_mutually_exclusive_group()
    platform.add_argument("--android", action="store_true")
    platform.add_argument("--ios", action="store_true")
    parser.add_argument("--simulator", action="store_true")
    options = parser.parse_args(arguments[1:])
    if options.simulator and not options.ios:
        parser.error("--simulator requires --ios")

    initializer = root / "deps/init/dep_create.sh"
    environment = os.environ.copy()
    environment["ANDROID_BUILD"] = "true" if options.android else "false"
    if options.ios:
        sdk = "iphonesimulator" if options.simulator else "iphoneos"
        if sys.platform != "darwin" or subprocess.run(
            ["xcrun", "--sdk", sdk, "--show-sdk-path"], env=environment,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode:
            raise LauncherError("iOS SDK unavailable; install Xcode and set DEVELOPER_DIR to its Contents/Developer directory")
    status = subprocess.run(
        ["bash", initializer.name],
        cwd=str(initializer.parent),
        env=environment,
    ).returncode
    if status or not options.ios:
        return status
    target = "aarch64-apple-ios-sim" if options.simulator else "aarch64-apple-ios"
    status = subprocess.run(
        ["rustup", "target", "add", target], cwd=str(root / "rust"), env=environment,
    ).returncode
    if status:
        return status
    return subprocess.run(
        [sys.executable, str(root / "deps/ios-build/build.py"), "--reuse", "--jobs", str(os.cpu_count() or 1)]
        + (["--simulator"] if options.simulator else []),
        cwd=str(root), env=environment,
    ).returncode


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
        epilog="Prepare project dependencies with: ./bazel.py init [--android | --ios [--simulator]]",
        allow_abbrev=False,
    )
    parser.add_argument("--build-dir", type=Path, default=Path("build_bazel"))
    parser.add_argument("--launcher-cache-dir", type=Path, default=_cache_directory())
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    options, startup_options = parser.parse_known_args(arguments)
    build_directory = (root / options.build_dir.expanduser()).resolve()
    if build_directory == root:
        raise LauncherError("--build-dir must not name the workspace root")
    cache = options.launcher_cache_dir.expanduser().resolve()
    version = (root / ".bazelversion").read_text().strip()
    bazel = ensure_bazel(root, cache, version)
    build_directory.mkdir(parents=True, exist_ok=True)

    # Only machine-specific paths belong here. Build policy lives in .bazelrc;
    # ordinary Bazel flags on the command line override these defaults.
    lines = [
        "common --downloader_config=" + _quote((root / "bazel/downloader.cfg").as_posix()),
        "build --disk_cache=" + _quote((_cache_directory() / "disk-cache").as_posix()),
        "build --symlink_prefix=" + _quote(build_directory.as_posix() + "/"),
    ]
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
    except (LauncherError, BootstrapError, OSError) as error:
        print("ERROR: %s" % error, file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
