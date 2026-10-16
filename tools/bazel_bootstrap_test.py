#!/usr/bin/env python3
"""Exercise pinned release downloads and verification without external traffic."""

import hashlib
import http.server
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlsplit

import bazel_bootstrap as bootstrap


PAYLOAD = b"a pinned release binary\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        path = urlsplit(self.path).path
        if path.startswith("/missing"):
            self.send_error(404)
            return
        data = PAYLOAD if path == "/good" or path.startswith("/bazel/9.3.0/") else b"corrupt download"
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

    def test_download_uses_environment_proxy(self):
        url = "http://bazel-bootstrap.invalid/good"
        with patch.dict(os.environ, {"http_proxy": self.base}, clear=True):
            result = bootstrap.download_binary(url, self.destination, self.checksum)
        self.assertEqual(result.read_bytes(), PAYLOAD)
        self.assertEqual(self.server.requests, [url])

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

    def write_manifest(self, source):
        root = Path(self.directory.name) / "project"
        (root / "bazel").mkdir(exist_ok=True, parents=True)
        manifest = {
            "version": "9.3.0",
            "base_url": source,
            "sha256": {host: self.checksum for host in (
                "linux-x86_64", "linux-arm64", "darwin-x86_64", "darwin-arm64",
                "windows-x86_64.exe", "windows-arm64.exe",
            )},
        }
        (root / "bazel/binaries.json").write_text(json.dumps(manifest))
        return root, manifest

    def test_all_platforms_use_the_configured_mirror(self):
        root, manifest = self.write_manifest(self.base + "/bazel/")
        cache = Path(self.directory.name) / "cache"
        for host in manifest["sha256"]:
            with self.subTest(host=host), patch.object(bootstrap, "host_platform", return_value=host):
                result = bootstrap.ensure_bazel(root, cache, "9.3.0")
                self.assertEqual(result.read_bytes(), PAYLOAD)
                self.assertEqual(result.name, "bazel-9.3.0-" + host)
        self.assertEqual(self.server.requests, [
            "/bazel/9.3.0/bazel-9.3.0-" + host for host in manifest["sha256"]
        ])

    def test_missing_mirror_file_fails_without_fallback(self):
        root, _ = self.write_manifest(self.base + "/missing")
        cache = Path(self.directory.name) / "cache"
        with patch.object(bootstrap, "host_platform", return_value="darwin-arm64"):
            with self.assertRaisesRegex(bootstrap.BootstrapError, "404"):
                bootstrap.ensure_bazel(root, cache, "9.3.0")
        self.assertEqual(self.server.requests, ["/missing/9.3.0/bazel-9.3.0-darwin-arm64"])

    def test_invalid_mirror_url_is_rejected_before_download(self):
        root, _ = self.write_manifest("file:///bazel")
        with self.assertRaisesRegex(bootstrap.BootstrapError, "base_url"):
            bootstrap.ensure_bazel(root, Path(self.directory.name) / "cache", "9.3.0")
        self.assertEqual(self.server.requests, [])


if __name__ == "__main__":
    unittest.main()
