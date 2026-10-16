#!/usr/bin/env python3
"""Exercise pinned release downloads and verification without external traffic."""

import gzip
import hashlib
import http.server
import io
import tarfile
import tempfile
import threading
import unittest
from pathlib import Path

import bazel_bootstrap as bootstrap


PAYLOAD = b"a pinned release binary\n"


def deb_package(binary):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w") as archive:
        member = tarfile.TarInfo("usr/bin/bazel-real")
        member.size = len(binary)
        archive.addfile(member, io.BytesIO(binary))
    payload = gzip.compress(data.getvalue(), mtime=0)
    header = "%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (
        "data.tar.gz/", "0", "0", "0", "100644", len(payload),
    )
    return b"!<arch>\n" + header.encode() + payload + (b"\n" if len(payload) % 2 else b"")


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        if self.path == "/missing":
            self.send_error(404)
            return
        if self.path == "/deb":
            data = deb_package(PAYLOAD)
        else:
            data = PAYLOAD if self.path == "/good" else b"corrupt download"
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class DownloadTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.destination = Path(self.directory.name) / "bazel"
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.requests = []
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.server.server_port
        self.checksum = hashlib.sha256(PAYLOAD).hexdigest()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def test_verified_release_is_installed(self):
        result = bootstrap.download_binary(
            self.base + "/good", self.destination, self.checksum
        )
        self.assertEqual(result.read_bytes(), PAYLOAD)
        self.assertEqual(list(self.destination.parent.iterdir()), [self.destination])

    def test_missing_release_fails_without_another_request(self):
        with self.assertRaisesRegex(bootstrap.BootstrapError, "404"):
            bootstrap.download_binary(self.base + "/missing", self.destination, self.checksum)
        self.assertEqual(self.server.requests, ["/missing"])
        self.assertEqual(list(self.destination.parent.iterdir()), [])

    def test_corrupt_release_is_never_installed(self):
        with self.assertRaisesRegex(bootstrap.BootstrapError, "SHA256 mismatch"):
            bootstrap.download_binary(self.base + "/bad", self.destination, self.checksum)
        self.assertEqual(list(self.destination.parent.iterdir()), [])

    def test_verified_cached_release_needs_no_source(self):
        self.destination.write_bytes(PAYLOAD)
        self.assertEqual(
            bootstrap.download_binary(self.base + "/missing", self.destination, self.checksum), self.destination
        )
        self.assertEqual(self.server.requests, [])

    def test_deb_mirror_installs_the_verified_native_binary(self):
        checksum = hashlib.sha256(deb_package(PAYLOAD)).hexdigest()
        result = bootstrap.download_deb_binary(
            self.base + "/deb",
            self.destination, self.checksum, checksum,
        )
        self.assertEqual(result.read_bytes(), PAYLOAD)
        self.assertEqual(list(self.destination.parent.iterdir()), [self.destination])

    def test_corrupt_deb_is_never_installed(self):
        with self.assertRaisesRegex(bootstrap.BootstrapError, "SHA256 mismatch"):
            bootstrap.download_deb_binary(
                self.base + "/deb", self.destination, self.checksum, "0" * 64,
            )
        self.assertEqual(list(self.destination.parent.iterdir()), [])

    def test_deb_binary_is_checked_against_the_official_binary_hash(self):
        checksum = hashlib.sha256(deb_package(PAYLOAD)).hexdigest()
        with self.assertRaisesRegex(bootstrap.BootstrapError, "SHA256 mismatch"):
            bootstrap.download_deb_binary(
                self.base + "/deb", self.destination, "0" * 64, checksum,
            )
        self.assertEqual(list(self.destination.parent.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
