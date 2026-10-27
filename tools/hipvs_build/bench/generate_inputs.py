#!/usr/bin/env python3
"""Portable fixed synthetic AMD experiment inputs. Default PLAN; no DB/GPU/network/client packaging."""
from array import array
import argparse
import hashlib
import heapq
import json
import math
from pathlib import Path
import struct
import sys
import time

class MT19937:
    def __init__(self, seed):
        self.state = [seed]
        for i in range(1, 624):
            prior = self.state[-1]
            self.state.append((1812433253 * (prior ^ (prior >> 30)) + i) & 0xFFFFFFFF)
        self.position = 624

    def next(self):
        if self.position == 624:
            for i in range(624):
                value = (self.state[i] & 0x80000000) | (self.state[(i + 1) % 624] & 0x7FFFFFFF)
                self.state[i] = self.state[(i + 397) % 624] ^ (value >> 1) ^ (0x9908B0DF if value & 1 else 0)
            self.position = 0
        value = self.state[self.position]
        self.position += 1
        value ^= value >> 11
        value ^= (value << 7) & 0x9D2C5680
        value ^= (value << 15) & 0xEFC60000
        value ^= value >> 18
        return value & 0xFFFFFFFF


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


EXPECTED_RAW_SHA256 = {
    "base.f32": "cc5ea4ca8bd02cdbf807f721df666ea797fdc16582d9ced4ba523f9e8c2e93e3",
    "queries.f32": "525d711d576289a68bf6648b522ac5f551e2ed633fa703998c7417f736629dc6",
    "ids.i64": "38e9a50ab1ed4f6339740f0c62ce81883911722ea190cf1b5e506c75202cdddc",
}


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.execute:
        print(json.dumps({"status": "PLAN_ONLY", "n": 20000, "dimension": 128, "queries": 24,
            "k": 10, "seed": 20261027, "writes_files": False, "starts_database_gpu_network": False,
            "algorithm": "Exact MT19937/IEEE float32 copied from the byte-verified original generator",
            "expected_raw_sha256": EXPECTED_RAW_SHA256}))
        return 0
    if args.output is None or not args.output.is_absolute():
        parser.error("Provide an explicit absolute, new, caller-owned output directory")
    output = args.output.resolve()
    if output.exists() or args.output.is_symlink() or not output.parent.is_dir():
        parser.error("Output must not exist; its caller-owned parent must already exist")
    control = MT19937(5489)
    assert [control.next() for _ in range(5)] == [3499211612, 581869302, 3890346734, 3586334585, 545404204]
    output.mkdir(mode=0o700, exist_ok=False)
    started = time.monotonic()
    generator = MT19937(20261027)
    below_one = struct.unpack("<f", struct.pack("<I", 0x3F7FFFFF))[0]
    def random_float():
        unit = f32(generator.next()) / 4294967296.0
        if unit >= 1.0:
            unit = below_one
        return f32(unit * 2.0 - 1.0)
    base = array("f", (random_float() for _ in range(20000 * 128)))
    queries = array("f", (random_float() for _ in range(24 * 128)))
    ids = array("q", (100000000003 + 17 * i for i in range(20000)))
    for name, values in (("base.f32", base), ("queries.f32", queries), ("ids.i64", ids)):
        encoded = array(values.typecode, values)
        if sys.byteorder != "little":
            encoded.byteswap()
        with (output / name).open("xb") as stream:
            stream.write(encoded.tobytes())
    raw_hashes = {name: digest(output / name) for name in EXPECTED_RAW_SHA256}
    if raw_hashes != EXPECTED_RAW_SHA256:
        (output / "raw-hash-mismatch.json").write_text(json.dumps(raw_hashes, indent=2) + "\n")
        raise ValueError("Generated input bytes differ from the recorded fixed experiment")
    truth = []
    for q in range(24):
        query = queries[q * 128:(q + 1) * 128]
        top = heapq.nsmallest(10, ((math.fsum((a - b) ** 2 for a, b in zip(base[i * 128:(i + 1) * 128], query)), ids[i])
                                  for i in range(20000)))
        truth.append({"query_id": q, "ids": [identity for _, identity in top]})
    (output / "exact-ground-truth.json").write_text(json.dumps(truth) + "\n")
    receipt = {"status": "GENERATED_RAW_SHA256_MATCH", "n": 20000, "dimension": 128, "queries": 24, "k": 10,
        "seed": 20261027, "raw_sha256": raw_hashes,
        "oracle_sha256": digest(output / "exact-ground-truth.json"),
        "elapsed_seconds": time.monotonic() - started,
        "database_gpu_network_install_or_client_packaging": False}
    (output / "generation-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
