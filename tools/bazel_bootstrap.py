"""Install the project's pinned Bazel binary from public release servers."""

import hashlib
import json
import os
import platform
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from typing import Iterable


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
                    for chunk in iter(lambda: response.read(1024 * 1024), b""):
                        output.write(chunk)
            if sha256_file(temporary) != checksum:
                raise BootstrapError("SHA256 mismatch for %s" % url)
            temporary.chmod(0o755)
            os.replace(str(temporary), str(destination))
            return destination
        except (OSError, ValueError, BootstrapError) as error:
            errors.append("%s: %s" % (url, error))
        finally:
            if temporary.exists():
                temporary.unlink()
    raise BootstrapError("cannot download the pinned Bazel binary:\n" + "\n".join(errors))


def _extract_deb_binary(package: Path, destination: Path) -> None:
    # A deb is an ar container. Read only Bazel's binary from its data tarball;
    # no package manager, install scripts or system-wide installation is needed.
    with package.open("rb") as stream:
        if stream.read(8) != b"!<arch>\n":
            raise BootstrapError("invalid Bazel deb package")
        while True:
            header = stream.read(60)
            if not header:
                break
            if len(header) != 60 or header[58:] != b"`\n":
                raise BootstrapError("invalid Bazel deb member header")
            size = int(header[48:58])
            if size < 0:
                raise BootstrapError("invalid Bazel deb member size")
            if header[:16].strip().rstrip(b"/") == b"data.tar.gz":
                with tarfile.open(fileobj=stream, mode="r|gz") as archive:
                    for member in archive:
                        if member.name in ("usr/bin/bazel-real", "./usr/bin/bazel-real") and member.isfile():
                            with archive.extractfile(member) as binary, destination.open("wb") as output:
                                shutil.copyfileobj(binary, output)
                            return
                break
            stream.seek(size + size % 2, os.SEEK_CUR)
    raise BootstrapError("Bazel binary is missing from the deb package")


def download_deb_binary(
    urls: Iterable[str], destination: Path, checksum: str, package_checksum: str,
) -> Path:
    if destination.is_file() and sha256_file(destination) == checksum:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".bazel-deb-", dir=str(destination.parent)) as directory:
        package = download_binary(urls, Path(directory) / "bazel.deb", package_checksum)
        binary = Path(directory) / "bazel"
        try:
            _extract_deb_binary(package, binary)
        except (OSError, ValueError, tarfile.TarError) as error:
            raise BootstrapError("cannot extract Bazel from deb: %s" % error) from error
        if sha256_file(binary) != checksum:
            raise BootstrapError("SHA256 mismatch for Bazel extracted from deb")
        binary.chmod(0o755)
        os.replace(str(binary), str(destination))
    return destination


def ensure_bazel(root: Path, cache_directory: Path, version: str) -> Path:
    manifest_path = root / "bazel" / "binaries.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        host = host_platform()
        checksum = manifest["sha256"][host]
        if manifest["version"] != version or not re.fullmatch(r"[0-9a-f]{64}", checksum):
            raise ValueError("version/checksum does not match .bazelversion")
        package = manifest.get("deb", {}).get(host)
        if package and (not re.fullmatch(r"[0-9a-f]{64}", package["sha256"]) or not package["urls"]):
            raise ValueError("invalid deb checksum/download sources")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise BootstrapError("invalid Bazel binary manifest %s: %s" % (manifest_path, error))
    filename = "bazel-%s-%s" % (version, host)
    destination = cache_directory / "binaries" / version / filename
    if package:
        return download_deb_binary(package["urls"], destination, checksum, package["sha256"])
    return download_binary(
        [
            "https://mirrors.huaweicloud.com/bazel/%s/%s" % (version, filename),
            "https://releases.bazel.build/%s/release/%s" % (version, filename),
        ],
        destination,
        checksum,
    )
