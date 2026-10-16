"""Install the project's pinned Bazel binary from public release servers."""

import hashlib
import json
import os
import platform
import re
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from typing import Iterable


DOWNLOAD_PROGRESS_INTERVAL = 5


class BootstrapError(RuntimeError):
    """A pinned Bazel binary could not be installed."""


def host_platform() -> str:
    system = {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}.get(
        platform.system()
    )
    machine = platform.machine().lower()
    architecture = {
        "x86_64": "x86_64", "amd64": "x86_64",
        "aarch64": "arm64", "arm64": "arm64",
    }.get(machine)
    if not system or not architecture:
        raise BootstrapError("unsupported Bazel host: %s %s" % (platform.system(), machine))
    return "%s-%s%s" % (system, architecture, ".exe" if system == "windows" else "")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_binary(urls: Iterable[str], destination: Path, checksum: str) -> Path:
    """Verify before atomic installation; release downloads never use a proxy."""
    if destination.is_file() and sha256_file(destination) == checksum:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    errors = []
    for url in urls:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=destination.name + ".", dir=str(destination.parent)
        )
        temporary = Path(temporary_name)
        try:
            print("Downloading Bazel without proxy: %s" % url, file=sys.stderr, flush=True)
            request = urllib.request.Request(url, headers={"User-Agent": "seekdb-bazel-bootstrap/1"})
            with os.fdopen(descriptor, "wb") as output:
                with opener.open(request, timeout=30) as response:
                    length = response.headers.get("Content-Length", "")
                    total = int(length) if length.isdigit() else 0
                    downloaded = 0
                    started = last_progress = time.monotonic()
                    # read1 returns available data without waiting to fill a
                    # large buffer on slow connections.
                    for chunk in iter(lambda: response.read1(64 * 1024), b""):
                        output.write(chunk)
                        downloaded += len(chunk)
                        now = time.monotonic()
                        if now - last_progress >= DOWNLOAD_PROGRESS_INTERVAL or downloaded == total:
                            progress = "Downloaded %.1f MiB" % (downloaded / (1024 * 1024))
                            if total:
                                progress += " / %.1f MiB (%.1f%%)" % (
                                    total / (1024 * 1024), downloaded * 100 / total,
                                )
                            speed = downloaded / max(now - started, 0.001) / 1024
                            print("%s, %.1f KiB/s" % (progress, speed), file=sys.stderr, flush=True)
                            last_progress = now
            if sha256_file(temporary) != checksum:
                raise BootstrapError("SHA256 mismatch for %s" % url)
            temporary.chmod(0o755)
            os.replace(str(temporary), str(destination))
            print("Bazel installed: %s" % destination, file=sys.stderr, flush=True)
            return destination
        except (OSError, ValueError, BootstrapError) as error:
            errors.append("%s: %s" % (url, error))
        finally:
            if temporary.exists():
                temporary.unlink()
    raise BootstrapError("cannot download the pinned Bazel binary:\n" + "\n".join(errors))


def ensure_bazel(root: Path, cache_directory: Path, version: str) -> Path:
    manifest_path = root / "bazel" / "binaries.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        host = host_platform()
        checksum = manifest["sha256"][host]
        if manifest["version"] != version or not re.fullmatch(r"[0-9a-f]{64}", checksum):
            raise ValueError("version/checksum does not match .bazelversion")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise BootstrapError("invalid Bazel binary manifest %s: %s" % (manifest_path, error))
    filename = "bazel-%s-%s" % (version, host)
    destination = cache_directory / "binaries" / version / filename
    return download_binary(
        [
            "https://mirrors.huaweicloud.com/bazel/%s/%s" % (version, filename),
            "https://releases.bazel.build/%s/release/%s" % (version, filename),
        ],
        destination,
        checksum,
    )
