#!/usr/bin/env python3
# Copyright (c) 2025 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Wire regressions for a disposable SeekDB test instance.

Geographic WKB cases require the default SRS fixture to be loaded first.
No connection is made unless --port is supplied. Authentication uses the
SEEKDB_TEST_USER (default root) and SEEKDB_TEST_PASSWORD environment variables.

Example: python3 unittest/observer/mysql/test_protocol_bounds.py --port 2889
"""

import argparse
import hashlib
import os
import socket
import struct
import unittest


CLIENT_PROTOCOL_41 = 1 << 9
CLIENT_SECURE_CONNECTION = 1 << 15
CLIENT_PLUGIN_AUTH = 1 << 19
CLIENT_CONNECT_ATTRS = 1 << 20
MYSQL_TYPE_VAR_STRING = 253
MYSQL_TYPE_COMPLEX = 160
ER_WRONG_ARGUMENTS = 1210
OB_ERR_MALFORMED_PS_PACKET = 9747
OPTIONS = None


def lenenc(value):
    if value < 251:
        return bytes([value])
    if value <= 0xFFFF:
        return b"\xfc" + struct.pack("<H", value)
    if value <= 0xFFFFFF:
        return b"\xfd" + value.to_bytes(3, "little")
    return b"\xfe" + struct.pack("<Q", value)


def field(value):
    return lenenc(len(value)) + value


def error_code(payload):
    if payload[:1] != b"\xff":
        return None
    return int.from_bytes(payload[1:3], "little")


class Client:
    def __init__(self):
        self.sock = socket.create_connection((OPTIONS.host, OPTIONS.port), timeout=5)
        self.sock.settimeout(5)

    def close(self):
        self.sock.close()

    def exact(self, length):
        chunks = bytearray()
        while len(chunks) < length:
            part = self.sock.recv(length - len(chunks))
            if not part:
                raise AssertionError("server closed before completing a response")
            chunks.extend(part)
        return bytes(chunks)

    def read(self):
        header = self.exact(4)
        length = int.from_bytes(header[:3], "little")
        if length > 1024 * 1024:
            raise AssertionError("unexpectedly large response to a bounds regression")
        return self.exact(length)

    def send(self, payload, sequence=0):
        self.sock.sendall(len(payload).to_bytes(3, "little") + bytes([sequence]) + payload)

    def login(self, version=b"1.2.3"):
        greeting = self.read()
        if not greeting or greeting[0] != 10:
            raise AssertionError("expected a MySQL protocol-10 greeting")
        pos = greeting.index(b"\0", 1) + 1 + 4
        salt = greeting[pos:pos + 8]
        pos += 9
        caps = int.from_bytes(greeting[pos:pos + 2], "little")
        pos += 2
        charset = greeting[pos]
        pos += 3
        caps |= int.from_bytes(greeting[pos:pos + 2], "little") << 16
        pos += 2
        auth_length = greeting[pos]
        pos += 11
        salt_part_length = max(13, auth_length - 8)
        salt += greeting[pos:pos + salt_part_length].rstrip(b"\0")
        pos += salt_part_length
        plugin = greeting[pos:].split(b"\0", 1)[0]
        if plugin not in (b"", b"mysql_native_password"):
            raise AssertionError("regression client requires mysql_native_password")
        required = CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION | CLIENT_CONNECT_ATTRS
        if caps & required != required:
            raise AssertionError("server does not advertise required protocol capabilities")
        requested = required | (caps & CLIENT_PLUGIN_AUTH)
        password = os.environ.get("SEEKDB_TEST_PASSWORD", "").encode()
        if password:
            # mysql_native_password requires this SHA-1 challenge response:
            # SHA1(password) XOR SHA1(nonce || SHA1(SHA1(password))).
            # This test client does not persist passwords or intermediate hashes.
            first = hashlib.sha1(password).digest()
            second = hashlib.sha1(first).digest()
            digest = hashlib.sha1(salt[:20] + second).digest()
            auth = bytes(a ^ b for a, b in zip(first, digest))
        else:
            auth = b""
        response = struct.pack("<IIB23x", requested, 1024 * 1024, charset)
        response += os.environ.get("SEEKDB_TEST_USER", "root").encode() + b"\0"
        response += bytes([len(auth)]) + auth
        if requested & CLIENT_PLUGIN_AUTH:
            response += b"mysql_native_password\0"
        attrs = field(b"__ob_client_version") + field(version)
        response += field(attrs)
        self.send(response, 1)
        return self.read()

    def prepare(self, sql):
        self.send(b"\x16" + sql)
        reply = self.read()
        if reply[:1] != b"\0":
            raise AssertionError("prepare failed, error code %s" % error_code(reply))
        statement_id, columns, parameters = struct.unpack_from("<IHH", reply, 1)
        for count in (parameters, columns):
            if count:
                for _ in range(count):
                    self.read()
                if self.read()[:1] != b"\xfe":
                    raise AssertionError("expected prepare metadata EOF")
        return statement_id

    def execute(self, statement_id, tail):
        self.send(b"\x17" + struct.pack("<IBI", statement_id, 0, 1) + tail)
        return self.read()

    def alive(self):
        self.send(b"\x03SELECT 1")
        reply, _ = self.result()
        if error_code(reply) is not None:
            raise AssertionError("connection is unusable after malformed request")

    def result(self, reply=None):
        reply = self.read() if reply is None else reply
        if reply[:1] in (b"\xff", b"\0"):
            return reply, []
        if not reply or reply[0] >= 251:
            raise AssertionError("unexpected result column count")
        for _ in range(reply[0]):
            self.read()
        if self.read()[:1] != b"\xfe":
            raise AssertionError("expected result metadata EOF")
        rows = []
        while True:
            packet = self.read()
            if packet[:1] == b"\xff":
                return packet, rows
            if packet[:1] == b"\xfe" and len(packet) < 9:
                return b"\0", rows
            rows.append(packet)

    def query(self, sql):
        self.send(b"\x03" + sql)
        return self.result()


class BaseFixture(unittest.TestCase):
    def connect(self, version=b"1.2.3"):
        client = Client()
        self.addCleanup(client.close)
        reply = client.login(version)
        self.assertEqual(b"\0", reply[:1], "login failed, error code %s" % error_code(reply))
        return client


class ProtocolBounds(BaseFixture):
    def test_normal_version(self):
        self.connect().alive()

    def test_ignored_long_version_suffix(self):
        self.connect(b"1.2.3." + b"x" * 4096).alive()

    def test_oversized_version_prefix_rejected(self):
        for length in (256, 4096):
            with self.subTest(length=length):
                client = Client()
                self.addCleanup(client.close)
                self.assertEqual(ER_WRONG_ARGUMENTS, error_code(client.login(b"1" * length)))
                self.connect().alive()

    def test_truncated_string_length_prefix(self):
        for marker in (252, 253, 254):
            with self.subTest(marker=marker):
                client = self.connect()
                statement_id = client.prepare(b"SELECT ?")
                tail = bytes([0, 1, MYSQL_TYPE_VAR_STRING, 0, marker])
                self.assertEqual(OB_ERR_MALFORMED_PS_PACKET, error_code(client.execute(statement_id, tail)))
                client.alive()

    def test_complex_null_cannot_disable_string_bounds(self):
        # Inline complex metadata: empty relation/name, version zero,
        # followed by an elementary string type. The complex value is NULL,
        # so both parameter orders reach the malformed ordinary string.
        complex_type = bytes([MYSQL_TYPE_COMPLEX, 0, 0, 0, 0, MYSQL_TYPE_VAR_STRING])
        string_type = bytes([MYSQL_TYPE_VAR_STRING, 0])
        for complex_first in (False, True):
            with self.subTest(complex_first=complex_first):
                client = self.connect()
                statement_id = client.prepare(b"SELECT ?, ?")
                bitmap = 1 if complex_first else 2
                types = complex_type + string_type if complex_first else string_type + complex_type
                tail = bytes([bitmap, 1]) + types + lenenc(1024)
                self.assertEqual(OB_ERR_MALFORMED_PS_PACKET, error_code(client.execute(statement_id, tail)))
                client.alive()

    def test_string_length_cannot_wrap_signed_check(self):
        for length in (1 << 32, (1 << 32) + 1, 1 << 63, 0xFFFFFFFFFFFFFFFF):
            with self.subTest(length=length):
                client = self.connect()
                statement_id = client.prepare(b"SELECT ?")
                tail = bytes([0, 1, MYSQL_TYPE_VAR_STRING, 0]) + lenenc(length)
                self.assertEqual(OB_ERR_MALFORMED_PS_PACKET, error_code(client.execute(statement_id, tail)))
                client.alive()

    def test_complex_null_preserves_valid_string(self):
        client = self.connect()
        statement_id = client.prepare(b"SELECT ?, ?")
        types = bytes([MYSQL_TYPE_VAR_STRING, 0, MYSQL_TYPE_COMPLEX, 0, 0, 0, 0, MYSQL_TYPE_VAR_STRING])
        reply, rows = client.result(client.execute(statement_id, b"\x02\x01" + types + field(b"hello")))
        self.assertIsNone(error_code(reply))
        self.assertEqual(1, len(rows))

    def test_truncated_complex_metadata_length(self):
        client = self.connect()
        statement_id = client.prepare(b"SELECT ?")
        tail = bytes([1, 1, MYSQL_TYPE_COMPLEX, 0, 254])
        self.assertEqual(OB_ERR_MALFORMED_PS_PACKET, error_code(client.execute(statement_id, tail)))
        client.alive()


class WkbStackBounds(BaseFixture):
    def check_wkb_depth(self, srid):
        client = self.connect()
        point = struct.pack("<BI2d", 1, 1, 0.0, 0.0)
        collection = struct.pack("<BII", 1, 7, 1)
        for depth, should_succeed in ((4, True), (16384, False)):
            with self.subTest(srid=srid, depth=depth):
                wkb = collection * depth + point
                sql = b"SELECT ST_GeomFromWKB(X'" + wkb.hex().encode() + b"'," + str(srid).encode() + b")"
                reply, _ = client.query(sql)
                if should_succeed:
                    self.assertIsNone(error_code(reply), "shallow WKB must remain valid")
                else:
                    self.assertIsNotNone(error_code(reply), "deep WKB must return a bounded error")
                client.alive()

    def test_cartesian_collection_stack_bound(self):
        self.check_wkb_depth(0)

    def test_geographic_collection_stack_bound(self):
        self.check_wkb_depth(4326)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    OPTIONS, unittest_args = parser.parse_known_args()
    unittest.main(argv=[__file__] + unittest_args)
