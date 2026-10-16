#!/usr/bin/env python3
"""Exercise release download fallback and verification without external traffic."""

import hashlib
import http.server
import tempfile
import threading
import unittest
from pathlib import Path

import bazel_bootstrap as bootstrap


PAYLOAD = b"a pinned release binary\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/missing":
            self.send_error(404)
            return
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
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.server.server_port
        self.checksum = hashlib.sha256(PAYLOAD).hexdigest()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def test_missing_mirror_falls_back_to_verified_release(self):
        result = bootstrap.download_binary(
            [self.base + "/missing", self.base + "/good"], self.destination, self.checksum
        )
        self.assertEqual(result.read_bytes(), PAYLOAD)
        self.assertEqual(list(self.destination.parent.iterdir()), [self.destination])

    def test_corrupt_release_is_never_installed(self):
        with self.assertRaisesRegex(bootstrap.BootstrapError, "SHA256 mismatch"):
            bootstrap.download_binary([self.base + "/bad"], self.destination, self.checksum)
        self.assertEqual(list(self.destination.parent.iterdir()), [])

    def test_verified_cached_release_needs_no_source(self):
        self.destination.write_bytes(PAYLOAD)
        self.assertEqual(
            bootstrap.download_binary([], self.destination, self.checksum), self.destination
        )


if __name__ == "__main__":
    unittest.main()
