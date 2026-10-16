#!/usr/bin/env python3
"""Exercise release download fallback and verification without external traffic."""

import hashlib
import http.server
import io
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

import bazel_bootstrap as bootstrap


PAYLOAD = b"a pinned release binary\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/missing":
            self.send_error(404)
            return
        data = PAYLOAD if self.path in ("/good", "/slow") else b"corrupt download"
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.path == "/slow":
            self.wfile.write(data[:8])
            self.wfile.flush()
            self.server.first_chunk_sent.set()
            self.server.finish_download.wait(timeout=5)
            data = data[8:]
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class DownloadTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.destination = Path(self.directory.name) / "bazel"
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.first_chunk_sent = threading.Event()
        self.server.finish_download = threading.Event()
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

    def test_slow_download_reports_progress_before_completion(self):
        progress_seen = threading.Event()

        class ProgressOutput(io.StringIO):
            def write(self, value):
                result = super().write(value)
                if "Downloaded " in value:
                    progress_seen.set()
                return result

        output = ProgressOutput()
        with mock.patch.object(bootstrap, "DOWNLOAD_PROGRESS_INTERVAL", 0, create=True):
            with redirect_stderr(output), ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(
                    bootstrap.download_binary,
                    [self.base + "/slow"], self.destination, self.checksum,
                )
                try:
                    self.assertTrue(self.server.first_chunk_sent.wait(timeout=2))
                    self.assertTrue(
                        progress_seen.wait(timeout=1),
                        "download is receiving data but reports no progress",
                    )
                    self.assertFalse(self.destination.exists())
                finally:
                    self.server.finish_download.set()
                    future.result(timeout=2)
        self.assertEqual(self.destination.read_bytes(), PAYLOAD)


if __name__ == "__main__":
    unittest.main()
