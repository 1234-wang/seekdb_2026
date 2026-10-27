#!/usr/bin/env python3
"""Linux UUID startup regression for issue #1480; uses the actual server binary.

Example:
  python3 tools/uuid_node_regression/check.py --binary /path/to/seekdb \
      --client /path/to/obclient --work-dir /tmp/uuid-node-check

Requires a C compiler and an otherwise unused local test environment. Each case
uses its own database directory and port; only processes started here are stopped.
The work directory must not already exist. Logs and results are retained there.
"""

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import uuid


def sql(client, port, query):
    result = subprocess.run(
        [str(client), "-h127.0.0.1", f"-P{port}", "-uroot", "-N", "-B",
         "--connect-timeout=2", "-e", query],
        text=True, capture_output=True, timeout=20,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return result.stdout.strip().splitlines()


def run_case(binary, client, library, root, mode):
    case_dir = root / mode
    case_dir.mkdir()
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    environment = os.environ.copy()
    environment.pop("SEEKDB_RESTRICT_MAC", None)
    if mode != "normal":
        environment["LD_PRELOAD"] = str(library)
        environment["SEEKDB_RESTRICT_MAC"] = mode
    command = [
        str(binary), "--nodaemon", f"--base-dir={case_dir}", f"--port={port}",
        "--parameter", "memory_budget=2G", "--parameter", "cpu_count=2",
        "--parameter", "datafile_size=1G", "--parameter", "datafile_maxsize=4G",
        "--parameter", "log_disk_size=2G", "--parameter", "enable_rpc_service=false",
        "--log-level=WARN",
    ]
    with (case_dir / "startup.stdout.log").open("w") as output:
        process = subprocess.Popen(command, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + 180
            while True:
                if process.poll() is not None:
                    if mode == "rng_fail":
                        assert process.returncode == 1
                        result = {"mode": mode, "sql_ready": False, "exit_code": 1,
                                  "partial_entropy_failure_not_masked": True}
                        print(json.dumps(result), flush=True)
                        return result
                    raise RuntimeError(f"{mode}: server exited {process.returncode}; see {case_dir}")
                try:
                    if sql(client, port, "SELECT 1;") == ["1"]:
                        break
                except (RuntimeError, subprocess.TimeoutExpired):
                    pass
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"{mode}: SQL not ready; see {case_dir}")
                time.sleep(0.5)
            assert mode != "rng_fail", "entropy failure was masked by successful startup"

            version = sql(client, port, "SELECT VERSION();")[0]
            rows = sql(client, port,
                       "CREATE DATABASE uuid_node_regression;"
                       "CREATE TABLE uuid_node_regression.smoke(id INT PRIMARY KEY, value VARCHAR(20));"
                       "INSERT INTO uuid_node_regression.smoke VALUES (1, 'uuid-ready');"
                       "SELECT value FROM uuid_node_regression.smoke WHERE id=1;")
            assert rows == ["uuid-ready"], (mode, rows)
            query = "SELECT UUID();" * 32
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                groups = list(pool.map(lambda _: sql(client, port, query), range(4)))
            values = [uuid.UUID(value) for group in groups for value in group]
            assert len(values) == 128
            assert len(set(values)) == 128, f"{mode}: duplicate UUIDs across concurrent connections"
            assert all(value.version == 1 and value.variant == uuid.RFC_4122 for value in values)
            nodes = {value.node for value in values}
            assert len(nodes) == 1, f"{mode}: UUID node changed within the process"
            node = next(iter(nodes))
            if mode != "normal":
                assert node & (1 << 40), f"{mode}: synthetic node is missing its multicast bit"
            else:
                hardware_nodes = {
                    int(address.read_text().strip().replace(":", ""), 16)
                    for address in Path("/sys/class/net").glob("*/address")
                    if address.parent.name != "lo"
                }
                assert node in hardware_nodes, "normal: successful hardware MAC discovery was not preserved"
            later = uuid.UUID(sql(client, port, "SELECT UUID();")[0])
            assert later.node == node and later not in values
            sql(client, port, "DROP DATABASE uuid_node_regression;")
            result = {"mode": mode, "sql_ready": True, "version": version,
                      "concurrent_uuids": 128, "unique": True, "uuid_version": 1,
                      "variant": "RFC_4122", "node": f"{node:012x}", "node_stable": True,
                      "hardware_mac_preserved": mode == "normal",
                      "synthetic_multicast_bit": bool(node & (1 << 40))}
            print(json.dumps(result), flush=True)
            return result
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    client = args.client.resolve(strict=True)
    root = args.work_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    library = root / "restrict_mac.so"
    subprocess.run(["cc", "-shared", "-fPIC", str(Path(__file__).with_name("restrict_mac.c")),
                    "-o", str(library), "-ldl"], check=True)
    results = []
    for mode in ("normal", "error", "no_mac", "flags", "hardware", "rng_short", "rng_fail"):
        results.append(run_case(binary, client, library, root, mode))
        (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
