"""Install the project's pinned Bazel binary from its configured mirror."""

import hashlib
import json
import os
import platform
import re
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path


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


def download_binary(url: str, destination: Path, checksum: str) -> Path:
    """Verify before atomic installation; use the environment's proxy settings."""
    if destination.is_file() and sha256_file(destination) == checksum:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    opener = urllib.request.build_opener()
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=destination.name + ".", dir=str(destination.parent)
    )
    temporary = Path(temporary_name)
    try:
        print("Downloading Bazel: %s" % url, file=sys.stderr, flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "seekdb-bazel-bootstrap/1"})
        with os.fdopen(descriptor, "wb") as output:
            with opener.open(request, timeout=30) as response:
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    output.write(chunk)
        if sha256_file(temporary) != checksum:
            raise BootstrapError("SHA256 mismatch for %s" % url)
        temporary.chmod(0o755)
        os.replace(str(temporary), str(destination))
        return destination
    except (OSError, ValueError) as error:
        raise BootstrapError("cannot download Bazel from %s: %s" % (url, error)) from error
    finally:
        if temporary.exists():
            temporary.unlink()


def ensure_bazel(root: Path, cache_directory: Path, version: str) -> Path:
    manifest_path = root / "bazel" / "binaries.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        host = host_platform()
        checksum = manifest["sha256"][host]
        if manifest["version"] != version or not re.fullmatch(r"[0-9a-f]{64}", checksum):
            raise ValueError("version/checksum does not match .bazelversion")
        base_url = manifest["base_url"]
        if not isinstance(base_url, str):
            raise ValueError("base_url must be an HTTP or HTTPS URL")
        source = urllib.parse.urlsplit(base_url)
        if source.scheme not in ("http", "https") or not source.hostname or source.query or source.fragment:
            raise ValueError("base_url must be an HTTP or HTTPS URL without a query or fragment")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise BootstrapError("invalid Bazel binary manifest %s: %s" % (manifest_path, error))
    filename = "bazel-%s-%s" % (version, host)
    destination = cache_directory / "binaries" / version / filename
    return download_binary(
        "%s/%s/%s" % (base_url.rstrip("/"), version, filename),
        destination,
        checksum,
    )
