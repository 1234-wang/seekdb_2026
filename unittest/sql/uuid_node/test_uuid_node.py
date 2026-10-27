#!/usr/bin/env python3
"""Linux startup/SQL regression for #1480; requires cc and PyMySQL.

Run against a compiled server, never an existing database. Every case creates
its own temporary data directory and shuts down its own server process.
"""
# Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
import argparse
import concurrent.futures
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
import uuid

import pymysql

OPTIONS = None


class UUIDNodeStartupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shared = tempfile.TemporaryDirectory(prefix="seekdb-uuid-node-", dir=OPTIONS.work_dir)
        cls.shim = Path(cls.shared.name) / "uuid_node_faults.so"
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-Wall", "-Wextra",
                        str(Path(__file__).with_name("uuid_node_faults.c")), "-ldl", "-o", str(cls.shim)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.shared.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=self.shared.name)
        self.base = Path(self.directory.name)
        self.process = None
        self.output = (self.base / "stdout.log").open("wb")
        self.addCleanup(self.cleanup_server)

    def cleanup_server(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        self.output.close()
        if not self._outcome.success:
            print("\nServer evidence:\n" + self.logs()[-8000:])
        self.directory.cleanup()

    def logs(self):
        files = [self.base / "stdout.log", *self.base.glob("data/log/*")]
        return "\n".join(p.read_text(errors="replace")[-200000:] for p in files if p.is_file())

    def connect(self):
        return pymysql.connect(host="127.0.0.1", port=self.port, user="root", password="",
                               connect_timeout=1, read_timeout=10, autocommit=True)

    def start(self, fault="", expect_failure=False):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        env = os.environ.copy()
        if fault:
            env["UUID_NODE_FAULT"] = fault
            env["LD_PRELOAD"] = str(self.shim)
        args = [str(OPTIONS.binary), "--nodaemon", "--base-dir", str(self.base / "data"),
                "--port", str(self.port), "--log-level", "WARN",
                "--parameter", "memory_budget=3G", "--parameter", "cpu_count=2",
                "--parameter", "datafile_size=512M", "--parameter", "datafile_maxsize=2G",
                "--parameter", "log_disk_size=2G", "--parameter", "max_syslog_file_count=2"]
        self.process = subprocess.Popen(args, cwd=self.base, env=env,
                                        stdout=self.output, stderr=subprocess.STDOUT)
        if expect_failure:
            code = self.process.wait(timeout=OPTIONS.timeout)
            self.assertNotEqual(code, 0)
            self.assertIn("UUID_NODE_TEST entropy", self.logs())
            return
        deadline = time.monotonic() + OPTIONS.timeout
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                connection = self.connect()
                connection.close()
                return
            except pymysql.MySQLError:
                time.sleep(0.25)
        self.fail("Server did not reach SQL readiness: " + str(self.process.poll()) + "\n" + self.logs()[-8000:])

    def uuids(self, amount=32):
        values = []
        with self.connect() as connection, connection.cursor() as cursor:
            for _ in range(amount):
                cursor.execute("SELECT UUID()")
                values.append(uuid.UUID(cursor.fetchone()[0]))
            cursor.execute("SELECT 1")
            self.assertEqual(cursor.fetchone(), (1,))
        return values

    def assert_valid(self, values, synthetic):
        self.assertEqual(len(set(values)), len(values))
        self.assertEqual(len({v.node for v in values}), 1)
        for value in values:
            self.assertEqual(value.version, 1)
            self.assertEqual(value.variant, uuid.RFC_4122)
            if synthetic:
                self.assertTrue(value.node & (1 << 40))

    def test_normal_mac_and_concurrent_sql(self):
        self.start()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            groups = list(pool.map(self.uuids, [32] * 4))
        values = [v for group in groups for v in group]
        self.assert_valid(values, synthetic=False)
        address = Path("/sys/class/net/eth0/address")
        if address.exists():
            self.assertEqual(values[0].node, int(address.read_text().strip().replace(":", ""), 16))
        else:
            self.assertTrue(values[0].node & (1 << 40))

    def test_enumeration_denied(self):
        self.start("enum-eperm")
        self.assert_valid(self.uuids(), synthetic=True)
        self.assertIn("UUID_NODE_TEST enum-eperm", self.logs())

    def test_socket_denied(self):
        self.start("socket-eperm")
        self.assert_valid(self.uuids(), synthetic=True)
        self.assertIn("UUID_NODE_TEST socket-eperm", self.logs())

    def test_zero_mac(self):
        self.start("zero-mac")
        self.assert_valid(self.uuids(), synthetic=True)

    def test_interrupted_and_short_entropy_reads(self):
        self.start("rng-short-eintr")
        self.assert_valid(self.uuids(), synthetic=True)

    def test_entropy_eof_is_fatal(self):
        self.start("rng-eof", expect_failure=True)

    def test_entropy_error_is_fatal(self):
        self.start("rng-error", expect_failure=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--case", help="Run one named test, for before/after checks")
    OPTIONS = parser.parse_args()
    OPTIONS.binary = OPTIONS.binary.resolve(strict=True)
    if OPTIONS.work_dir:
        OPTIONS.work_dir.mkdir(parents=True, exist_ok=True)
    names = ["UUIDNodeStartupTest." + OPTIONS.case] if OPTIONS.case else ["UUIDNodeStartupTest"]
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromNames(names, __import__(__name__)))
    raise SystemExit(not result.wasSuccessful())
