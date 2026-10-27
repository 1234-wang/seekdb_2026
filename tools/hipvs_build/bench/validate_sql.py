#!/usr/bin/env python3
"""Plan or explicitly validate AMD snapshot construction in an already-owned private seekdb instance.

This program never starts/stops a server, builds code, installs packages, or accesses an existing user schema.
"""
from __future__ import annotations

import argparse
from array import array
import hashlib
import heapq
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import socket
import struct
import sys
import time
import traceback
import uuid


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def ident(value):
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,63}", value):
        raise ValueError("only runner-owned identifiers are allowed")
    return "`" + value + "`"


def vector_text(values):
    # Nine significant decimal digits round-trip every finite float32. Preserve signed zero as a float token.
    return "[" + ",".join(("-0.0" if math.copysign(1, x) < 0 else "0.0") if x == 0 else format(x, ".9g")
                          for x in values) + "]"


def valid_root(path):
    if _AUTHORIZED_ROOT is None:
        raise ValueError("an explicit owned --workspace-root is required for execution")
    candidate = Path(path)
    if not candidate.is_absolute():
        raise ValueError("execution paths must be absolute")
    resolved = candidate.resolve()
    if not resolved.is_relative_to(_AUTHORIZED_ROOT):
        raise ValueError("execution paths must stay within the explicitly owned workspace")
    return resolved


def process_identity(args, original=None, with_hash=False):
    proc = Path("/proc") / str(args.pid)
    stat = (proc / "stat").read_text()
    start = int(stat[stat.rfind(")") + 2:].split()[19])
    exe = (proc / "exe").resolve(strict=True)
    expected = Path(args.binary).resolve(strict=True)
    current = exe.stat()
    if exe != expected or (proc / "cwd").resolve(strict=True) != Path(args.base_dir).resolve(strict=True):
        raise ValueError("server executable/base directory mismatch")
    uid_line = next(line for line in (proc / "status").read_text().splitlines() if line.startswith("Uid:"))
    actual_uid = int(uid_line.split()[2])
    if actual_uid != args.expected_uid:
        raise ValueError("server effective UID differs from the owned instance")
    identity = {"pid": args.pid, "start_ticks": start, "exe": str(exe), "uid": actual_uid,
                "device": current.st_dev, "inode": current.st_ino, "bytes": current.st_size,
                "mtime_ns": current.st_mtime_ns, "base_dir": str(Path(args.base_dir).resolve()),
                "host": args.host, "port": args.port}
    if original is not None and identity != {key: original[key] for key in identity}:
        raise ValueError("server identity changed while validating")
    if args.database_start_ticks is not None and start != args.database_start_ticks:
        raise ValueError("server start ticks mismatch with the owning controller")
    if with_hash and digest(exe) != args.expected_sha256:
        raise ValueError("actual server binary SHA256 mismatch")
    # Inspect only this process's descriptors/socket metadata; no unrelated file contents or command-line credentials.
    sockets = set()
    for fd in (proc / "fd").iterdir():
        try:
            match = re.fullmatch(r"socket:\[(\d+)\]", os.readlink(fd))
            if match:
                sockets.add(match[1])
        except FileNotFoundError:
            pass
    addresses = {str(ipaddress.ip_address(args.host))} if args.host != "localhost" else {"127.0.0.1", "::1"}
    if os.readlink(proc / "ns/net") != os.readlink("/proc/self/ns/net"):
        raise ValueError("SQL runner must share the owned private network namespace")
    import fcntl
    local_addresses = {"127.0.0.1", "::1", "0.0.0.0", "::"}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        for _, name in socket.if_nameindex():
            try:
                raw_address = fcntl.ioctl(probe.fileno(), 0x8915, struct.pack("256s", name.encode()[:15]))
                local_addresses.add(socket.inet_ntoa(raw_address[20:24]))
            except OSError:
                pass
    if Path("/proc/net/if_inet6").exists():
        for line in Path("/proc/net/if_inet6").read_text().splitlines():
            local_addresses.add(str(ipaddress.IPv6Address(bytes.fromhex(line.split()[0]))))
    if not addresses <= local_addresses:
        raise ValueError("HOST must address this same private namespace, never an unrelated remote database")
    listener = False
    for name in ("tcp", "tcp6"):
        for line in (proc / "net" / name).read_text().splitlines()[1:]:
            fields = line.split()
            host_hex, port_hex = fields[1].split(":")
            if int(port_hex, 16) != args.port or fields[3] != "0A" or fields[9] not in sockets:
                continue
            raw = bytes.fromhex(host_hex)
            bound = socket.inet_ntop(socket.AF_INET if name == "tcp" else socket.AF_INET6,
                                     raw[::-1] if name == "tcp" else b"".join(raw[i:i + 4][::-1] for i in range(0, 16, 4)))
            if bound in addresses or bound in ("0.0.0.0", "::"):
                listener = True
    if not listener:
        raise ValueError("expected private SQL port is not owned by the given server PID")
    selected = {}
    allowed = {b"SEEKDB_HIPVS_BUILD_HELPER", b"SEEKDB_HIPVS_BUILD_TMPDIR", b"SEEKDB_HIPVS_BUILD_TIMEOUT_SECONDS"}
    for part in (proc / "environ").read_bytes().split(b"\0"):
        key, sep, value = part.partition(b"=")
        if sep and key in allowed:
            selected[key.decode("ascii")] = value.decode("utf-8")
    helper = selected.get("SEEKDB_HIPVS_BUILD_HELPER", "")
    if args.mode == "cpu" and helper:
        raise ValueError("CPU-off mode unexpectedly has the helper enabled")
    if args.mode != "cpu":
        if not helper or Path(helper).resolve() != Path(args.expected_helper).resolve():
            raise ValueError("actual helper environment does not match the explicit test mode")
        if args.mode == "gpu" and digest(helper) != args.expected_helper_sha256:
            raise ValueError("GPU helper binary SHA mismatch")
    return identity, selected


def load_inputs(root):
    root = valid_root(root)
    base, queries, ids = array("f"), array("f"), array("q")
    for name, target in (("base.f32", base), ("queries.f32", queries), ("ids.i64", ids)):
        target.frombytes((root / name).read_bytes())
    if sys.byteorder != "little":
        for value in (base, queries, ids):
            value.byteswap()
    if len(ids) != 20000 or len(base) != 20000 * 128 or len(queries) != 24 * 128:
        raise ValueError("this controlled integration experiment requires the unchanged R3 20000x128/24-query workload")
    if len(set(ids)) != len(ids) or any(not math.isfinite(x) for x in base) or any(not math.isfinite(x) for x in queries):
        raise ValueError("invalid original IDs or non-finite vectors")
    truth_raw = json.loads((root / "exact-ground-truth.json").read_text())
    truth = {int(row["query_id"]): tuple(int(x) for x in row["ids"]) for row in truth_raw}
    if set(truth) != set(range(24)) or any(len(v) != 10 or len(set(v)) != 10 or not set(v) <= set(ids) for v in truth.values()):
        raise ValueError("R3 exact oracle cardinality/IDs invalid")
    hashes = {name: digest(root / name) for name in ("base.f32", "queries.f32", "ids.i64", "exact-ground-truth.json")}
    return root, base, queries, ids, truth, hashes


def exact(base, query, labels, rows):
    return tuple(identity for _, identity in heapq.nsmallest(10,
        ((math.fsum((float(a) - float(b)) ** 2 for a, b in zip(base[row * 128:(row + 1) * 128], query)), identity)
         for row, identity in rows)))


def capture_round_backend(log, before, output, number):
    """Hash the exact new log span and retain bounded backend excerpts, never an older round's marker."""
    flags = {"gpu_native_import_trace": b"GPU graph imported with native original IDs; no CPU Build",
             "explicit_cpu_fallback_trace": b"GPU build not successful; explicit CPU fallback Build once"}
    found = {name: False for name in flags}
    source_hash = hashlib.sha256()
    snippets, critical_snippets, tail, scanned = [], {}, b"", 0
    descriptor = os.open(log, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        current = os.fstat(stream.fileno())
        if (current.st_dev, current.st_ino) != (before["device"], before["inode"]) or current.st_size < before["offset"]:
            raise ValueError("owned server log rotated/replaced/truncated during this round; backend not proven")
        end = current.st_size
        stream.seek(before["offset"])
        remaining = end - before["offset"]
        while remaining:
            data = stream.read(min(65536, remaining))
            if not data:
                raise ValueError("owned server log shortened while capturing the round")
            source_hash.update(data)
            scanned += len(data)
            remaining -= len(data)
            window = tail + data
            for name, marker in flags.items():
                position = window.find(marker)
                if position >= 0:
                    if not found[name]:
                        critical_snippets[name] = window[max(0, position - 256):position + 2048]
                    found[name] = True
            for marker in (b"[OBVSAG]", b"[hipVS]"):
                position = window.find(marker)
                if position >= 0 and len(snippets) < 64:
                    snippets.append(window[max(0, position - 256):position + 2048])
            tail = window[-256:]
        if os.fstat(stream.fileno()).st_size < end:
            raise ValueError("owned server log was truncated after capture")
    path = output / ("round-" + str(number) + "-server-trace.log")
    path.write_bytes(b"\n[bounded backend excerpts; full new log span SHA is in report]\n".join(
        list(critical_snippets.values()) + snippets))
    return {**found, "device": before["device"], "inode": before["inode"],
            "start_offset": before["offset"], "end_offset": end, "new_span_bytes": scanned,
            "new_span_sha256": source_hash.hexdigest(), "excerpt_file": path.name,
            "excerpt_bytes": path.stat().st_size, "excerpts_are_not_full_log": True}


_AUTHORIZED_ROOT = None

def main():
    global _AUTHORIZED_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--workspace-root", type=Path, help="Existing absolute workspace owned by this user; no default execution root")
    parser.add_argument("--disposable", action="store_true")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--resume", type=Path, help="Read-only verification after ROOT normally restarts the same owned DB")
    parser.add_argument("--client-zip", type=Path)
    parser.add_argument("--mode", choices=("cpu", "helper-fail", "gpu"))
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int)
    parser.add_argument("--pid", "--database-pid", dest="pid", type=int)
    parser.add_argument("--database-start-ticks", type=int)
    parser.add_argument("--expected-uid", type=int)
    parser.add_argument("--binary")
    parser.add_argument("--expected-sha256")
    parser.add_argument("--expected-revision")
    parser.add_argument("--expected-helper")
    parser.add_argument("--expected-helper-sha256")
    parser.add_argument("--base-dir")
    parser.add_argument("--server-log")
    parser.add_argument("--inputs")
    parser.add_argument("--output")
    parser.add_argument("--rounds", type=int)
    parser.add_argument("--deadline-seconds", type=int)
    args = parser.parse_args()
    plan = {"status": "PLAN_ONLY", "starts_server": False, "connects_database": False,
            "modes": ["cpu: helper ENV absent", "helper-fail: exact explicit helper and CPU fallback trace", "gpu: sealed helper + DDL GPU native-import trace"],
            "required_config": ["workspace_root", "mode", "host", "port", "pid", "expected_uid", "binary", "expected_sha256", "expected_revision", "base_dir", "server_log", "inputs", "output"],
            "data": "unchanged R3 20000x128 float32, 24 queries, noncontiguous original SQL IDs; decimal .9g roundtrip",
            "tests": ["independent schema and all original IDs", "3 fresh offline indexes timed through first ANN", "all 24 queries/complete exact Recall", "filtered native ANN", "rollback/commit vector changes, delete/insert/PK change", "root-controlled restart then read-only resume"],
            "official_scope": "formal seekdb PR + build/dependency/data/repro + 3 full CPU/GPU rebuild medians/ID/Recall/GPU/fallback evidence; no unpublished threshold invented",
            "gpu_claim_gate": "matching private PID/binary/helper identity AND DDL-time server log native-GPU success, not standalone helper records"}
    if not args.execute:
        print(json.dumps(plan, ensure_ascii=False, indent=2))
        return 0
    if not args.disposable:
        parser.error("--execute requires --disposable; ROOT owns the existing private instance lifecycle")
    if args.workspace_root is None or not args.workspace_root.is_absolute():
        parser.error("--execute requires an absolute owned --workspace-root")
    _AUTHORIZED_ROOT = args.workspace_root.resolve(strict=True)
    if not _AUTHORIZED_ROOT.is_dir() or not hasattr(os, "geteuid") or _AUTHORIZED_ROOT.stat().st_uid != os.geteuid():
        parser.error("workspace must be an existing directory owned by the executing Linux user")
    if args.config:
        config = json.loads(valid_root(args.config).read_text())
        accepted = {key for key in vars(args) if key not in ("execute", "disposable", "config", "workspace_root")}
        if set(config) - accepted:
            parser.error("unknown config fields")
        for key, value in config.items():
            if getattr(args, key) is None:
                setattr(args, key, value)
    args.host = args.host or "127.0.0.1"
    args.expected_uid = os.geteuid() if args.expected_uid is None else args.expected_uid
    args.deadline_seconds = args.deadline_seconds or 900
    if args.pid is not None and not args.binary:
        args.binary = str((Path("/proc") / str(args.pid) / "exe").resolve(strict=True))
    if args.base_dir and not args.server_log:
        args.server_log = str(Path(args.base_dir) / "log/seekdb.log")
    if not args.mode or any(getattr(args, key) is None for key in plan["required_config"] if key not in ("mode",)):
        parser.error("all required private-instance config fields must be supplied")
    if args.port in (2881, 2883) or not 1024 <= int(args.port) <= 65535:
        parser.error("use a dedicated private SQL port; user instance ports 2881/2883 are excluded")
    if not re.fullmatch(r"[0-9a-f]{64}", args.expected_sha256) or not re.fullmatch(r"[0-9a-f]{40}", args.expected_revision):
        parser.error("full expected binary SHA256 and source revision are required")
    if args.mode != "cpu" and not args.expected_helper:
        parser.error("enabled/failure mode requires an exact expected helper path")
    if args.mode == "gpu" and not args.expected_helper_sha256:
        parser.error("GPU mode requires the expected helper binary SHA256")
    args.base_dir = str(valid_root(args.base_dir))
    log = valid_root(args.server_log)
    if not log.is_relative_to(Path(args.base_dir)):
        parser.error("server log must belong to this owned base directory")
    output = valid_root(args.output)
    output.mkdir(mode=0o700, exist_ok=False)
    report = {"schema_version": 1, "status": "RUNNING", "mode": args.mode, "database_connected": False,
              "checks": [], "build_rounds": [], "not_run": ["root_controlled_normal_restart_readback"],
              "official_acceptance": False, "local_quality_floor": 0.95,
              "quality_floor_is_official_threshold": False}
    started, connection = time.monotonic(), None
    deadline = started + args.deadline_seconds
    journal = (output / "sql.jsonl").open("w", encoding="utf-8")
    failures = []
    def save():
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    def check(label, condition, details=None):
        report["checks"].append({"label": label, "status": "PASSED" if condition else "FAILED", "details": details})
        save()
        if not condition:
            raise AssertionError(label)
    def query(label, statement, parameters=None, bulk=False):
        if time.monotonic() >= deadline:
            raise TimeoutError("overall validation deadline")
        entry = {"label": label, "sql": statement if not bulk else None,
                 "sql_sha256": hashlib.sha256(statement.encode()).hexdigest(), "bytes": len(statement.encode())}
        begin = time.monotonic()
        try:
            with connection.cursor() as cursor:
                cursor.execute(statement, parameters)
                rows = cursor.fetchall() if cursor.description else ()
            entry.update(status="OK", seconds=time.monotonic() - begin, rows=len(rows))
            return rows
        except Exception as error:
            entry.update(status="FAILED", seconds=time.monotonic() - begin, error=f"{type(error).__name__}: {error}")
            raise
        finally:
            journal.write(json.dumps(entry, ensure_ascii=False) + "\n"); journal.flush()
    try:
        identity, environment = process_identity(args, with_hash=True)
        report.update(server_identity=identity, helper_environment=environment)
        root, base, queries, ids, truth, hashes = load_inputs(args.inputs)
        report.update(inputs=str(root), input_sha256=hashes)
        if args.client_zip:
            sys.path.insert(0, str(valid_root(args.client_zip)))
        import pymysql
        connection = pymysql.connect(host=args.host, port=args.port, user="root", charset="utf8mb4", autocommit=True,
                                     connect_timeout=5, read_timeout=180, write_timeout=180)
        report["database_connected"] = True
        version = query("source revision", "SELECT @@version_comment, VERSION()")
        check("actual SQL server revision", args.expected_revision in str(version), version)
        query("set actual HNSW session search width", "SET SESSION ob_hnsw_ef_search=512")
        search_width = query("read actual HNSW session search width", "SELECT @@ob_hnsw_ef_search")
        check("actual query ef_search equals component CPU512", int(search_width[0][0]) == 512, search_width)
        report["query_ef_search"] = int(search_width[0][0])
        if args.resume:
            previous = json.loads(valid_root(args.resume).read_text())
            if previous.get("status") != "PASSED_FUNCTIONAL_REOPEN_PENDING" or previous["input_sha256"] != hashes:
                raise ValueError("resume needs a successful owned report and unchanged inputs")
            if previous["server_identity"]["base_dir"] != identity["base_dir"] or previous["server_identity"]["start_ticks"] == identity["start_ticks"]:
                raise ValueError("resume requires a newly started process on the same owned persistent base directory")
            schema = previous["schema"]
        else:
            schema = "amd_validation_" + uuid.uuid4().hex[:16]
            query("create owned schema", "CREATE DATABASE " + ident(schema))
        report["schema"] = schema
        table = ident(schema) + ".`docs`"
        def ann(label, vector, where="", limit=10):
            statement = "SELECT id FROM " + table + where + " ORDER BY l2_distance(embedding,%s) APPROXIMATE LIMIT " + str(limit)
            return tuple(int(row[0]) for row in query(label, statement, (vector_text(vector),)))
        def rows_recall(label, expected, returned, allowed):
            check(label + " cardinality/original IDs", len(returned) == 10 and len(set(returned)) == 10 and set(returned) <= set(allowed), list(returned))
            return len(set(expected) & set(returned)) / 10
        if args.resume:
            state = previous["dml_state"]
            expected_ids = set(ids) - {state["deleted_id"]}
            expected_ids.add(state["inserted_final_id"])
            actual_ids = tuple(int(row[0]) for row in query("all persistent original IDs", "SELECT id FROM " + table + " ORDER BY id"))
            check("all original SQL IDs survive normal restart", actual_ids == tuple(sorted(expected_ids)))
            check("updated vector survives normal restart", query("persistent updated vector", "SELECT l2_distance(embedding,%s) FROM " + table + " WHERE id=%s", (vector_text(base[128:256]), int(ids[0])))[0][0] == 0)
            check("new/PK-changed vector survives normal restart", ann("persistent inserted ANN", base[256:384], limit=1) == (state["inserted_final_id"],))
            check("deleted row remains absent after restart", query("persistent deleted row", "SELECT COUNT(*) FROM " + table + " WHERE id=%s", (state["deleted_id"],))[0][0] == 0)
            # Rebuild only the independent oracle, never the index: the deleted row's vector was reinserted under final_id.
            current_base, current_ids = array("f", base), list(ids)
            current_base[:128] = base[128:256]
            current_ids[2] = state["inserted_final_id"]
            reopen_hits, reopen_results = 0, []
            for q in range(24):
                expected = exact(current_base, queries[q * 128:(q + 1) * 128], current_ids, enumerate(current_ids))
                found = ann("reopen full query " + str(q), queries[q * 128:(q + 1) * 128])
                reopen_hits += rows_recall("reopen query " + str(q), expected, found, current_ids) * 10
                reopen_results.append({"query_id": q, "ids": list(found)})
            report["reopen_complete_recall"] = reopen_hits / 240
            check("reopen full 24-query Recall >= supplemental .95", report["reopen_complete_recall"] >= 0.95)
            (output / "reopen-results.json").write_text(json.dumps(reopen_results) + "\n")
            report["not_run"] = []
            report["status"] = "PASSED_READ_ONLY_REOPEN_COMPONENT"
        else:
            query("create raw table before any index", "CREATE TABLE " + table + "(id BIGINT PRIMARY KEY,group_id INT,embedding VECTOR(128))")
            for begin in range(0, len(ids), 500):
                values = ",".join("(" + str(ids[i]) + "," + str(ids[i] % 3) + ",'" + vector_text(base[i * 128:(i + 1) * 128]) + "')" for i in range(begin, min(begin + 500, len(ids))))
                query("insert raw batch " + str(begin), "INSERT INTO " + table + " VALUES " + values, bulk=True)
            actual_ids = tuple(int(row[0]) for row in query("all input original IDs", "SELECT id FROM " + table + " ORDER BY id"))
            check("all 20000 noncontiguous original SQL IDs stored", actual_ids == tuple(sorted(ids)))
            # Verify the independent R3 oracle is bound to these exact raw values for two whole-dataset queries.
            for q in range(2):
                check("R3 exact oracle raw binding " + str(q), exact(base, queries[q * 128:(q + 1) * 128], ids, enumerate(ids)) == truth[q])
            rounds = args.rounds or (1 if args.mode == "helper-fail" else 3)
            if not 1 <= rounds <= 3:
                raise ValueError("focused runner permits 1..3 fresh builds")
            last_index = None
            for number in range(rounds):
                index = "idx_amd_" + str(number)
                round_info = {"round": number, "index": index, "status": "RUNNING"}
                report["build_rounds"].append(round_info)
                log_identity = log.stat()
                before_log = {"device": log_identity.st_dev, "inode": log_identity.st_ino, "offset": log_identity.st_size}
                try:
                    process_identity(args, identity)
                    build_start = time.monotonic()
                    query("fresh offline CREATE VECTOR INDEX " + str(number), "CREATE VECTOR INDEX " + ident(index) + " ON " + table
                          + "(embedding) WITH(distance=l2,type=hnsw,lib=vsag,m=32,ef_construction=200,ef_search=512,extra_info_max_size=0)")
                    round_info["create_succeeded"] = True
                    first = ann("first query after fresh DDL " + str(number), queries[:128])
                    round_info["ddl_through_first_query_seconds"] = time.monotonic() - build_start
                    result_rows, hits = [], 0
                    for q in range(24):
                        found = first if q == 0 else ann("round " + str(number) + " query " + str(q), queries[q * 128:(q + 1) * 128])
                        hits += rows_recall("round " + str(number) + " query " + str(q), truth[q], found, ids) * 10
                        result_rows.append({"query_id": q, "ids": list(found)})
                    round_info["recall"] = hits / 240
                    (output / ("round-" + str(number) + "-results.json")).write_text(json.dumps(result_rows) + "\n")
                    backend_evidence = capture_round_backend(log, before_log, output, number)
                    round_info["backend_log_evidence"] = backend_evidence
                    imported = backend_evidence["gpu_native_import_trace"]
                    fallback = backend_evidence["explicit_cpu_fallback_trace"]
                    round_info.update(gpu_native_import_trace=imported, explicit_cpu_fallback_trace=fallback)
                    check("round " + str(number) + " complete query Recall >= supplemental .95", round_info["recall"] >= 0.95, round_info["recall"])
                    check("round " + str(number) + " actual DDL backend", (imported and not fallback) if args.mode == "gpu"
                          else (fallback and not imported) if args.mode == "helper-fail" else not imported and not fallback, round_info)
                    explain = query("native ANN plan " + str(number), "EXPLAIN SELECT id FROM " + table + " ORDER BY l2_distance(embedding,%s) APPROXIMATE LIMIT 10", (vector_text(queries[:128]),))
                    (output / ("round-" + str(number) + "-explain.json")).write_text(json.dumps(explain, default=str) + "\n")
                    check("round " + str(number) + " owned native index appears in plan", index in str(explain), explain)
                    round_info["status"] = "PASSED"
                    last_index = index
                except Exception as error:
                    round_info.update(status="FAILED", error=f"{type(error).__name__}: {error}")
                    failures.append(round_info["error"])
                    connection.rollback()
                    process_identity(args, identity)
                finally:
                    if "backend_log_evidence" not in round_info:
                        try:
                            round_info["backend_log_evidence"] = capture_round_backend(log, before_log, output, number)
                        except Exception as capture_error:
                            round_info["backend_log_capture_error"] = str(capture_error)
                    if number != rounds - 1 or round_info["status"] != "PASSED":
                        try:
                            query("drop only own fresh index " + str(number), "DROP INDEX " + ident(index) + " ON " + table)
                        except pymysql.MySQLError as cleanup_error:
                            if round_info["status"] != "FAILED" or not cleanup_error.args or cleanup_error.args[0] not in (1051, 1091, 1146):
                                raise
                            round_info["cleanup_observation"] = "failed CREATE left no index to drop"
                    save()
            if not last_index or report["build_rounds"][-1]["status"] != "PASSED":
                raise RuntimeError("no final successful own index for further independent DML checks")
            filtered_scores = []
            for q in range(2):
                expected = exact(base, queries[q * 128:(q + 1) * 128], ids, ((i, int(v)) for i, v in enumerate(ids) if v % 3 == 1))
                found = ann("filtered ANN " + str(q), queries[q * 128:(q + 1) * 128], " WHERE group_id=1")
                score = rows_recall("filtered ANN " + str(q), expected, found, [v for v in ids if v % 3 == 1])
                filtered_scores.append(score)
            check("complete filtered Recall >= supplemental .95", sum(filtered_scores) / 2 >= 0.95, filtered_scores)
            # Observe actual native VID shape when the public virtual generated column is available.
            try:
                metadata = query("own native VID metadata", "SELECT c.column_name FROM oceanbase.__all_column c"
                    " JOIN oceanbase.__all_table t ON t.table_id=c.table_id"
                    " JOIN oceanbase.__all_database d ON d.database_id=t.database_id"
                    " WHERE d.database_name=%s AND t.table_name='docs' ORDER BY c.column_id", (schema,))
                columns = sorted({row[0] for row in metadata if isinstance(row[0], str) and row[0].startswith("__vid_")})
                observed = []
                for name in columns[:2]:
                    if "\0" in name:
                        raise ValueError("NUL in VID metadata identifier")
                    native = query("own virtual VID values", "SELECT id,`" + name.replace("`", "``") + "` FROM " + table + " ORDER BY id")
                    values = [int(row[1]) for row in native if row[1] is not None]
                    observed.append({"column": name, "rows": len(native), "distinct": len(set(values)),
                                     "matches_dense_offsets": set(values) == set(range(len(ids))),
                                     "sample": [list(row) for row in native[:8]]})
                report["native_vid_observation"] = {"status": "OBSERVED" if columns else "PUBLIC_VID_COLUMN_UNAVAILABLE", "columns": observed,
                    "not_claimed_as_pass_if_unavailable": True, "native_signed64_relabel_component": "separately passed R5"}
            except Exception as observation_error:
                report["native_vid_observation"] = {"status": "OBSERVATION_ERROR", "error": str(observation_error), "acceptance_pass_claimed": False}
            # Transactions stay on this one real connection; no fake BEGIN across separate CLI processes.
            original = int(ids[0])
            check("original self row is indexed", ann("self before rollback", base[:128], limit=1) == (original,))
            query("start rollback validation", "START TRANSACTION")
            query("uncommitted NULL", "UPDATE " + table + " SET embedding=NULL WHERE id=%s", (original,))
            check("uncommitted NULL read", query("uncommitted NULL state", "SELECT embedding IS NULL FROM " + table + " WHERE id=%s", (original,))[0][0] == 1)
            check("uncommitted NULL removed from ANN", original not in ann("ANN while NULL", base[:128]))
            query("rollback own update", "ROLLBACK")
            check("rollback restores vector ANN", ann("self after rollback", base[:128], limit=1) == (original,))
            query("commit changed vector", "UPDATE " + table + " SET embedding=%s WHERE id=%s", (vector_text(base[128:256]), original))
            check("committed changed vector is searchable", original in ann("changed vector ANN", base[128:256]))
            deleted = int(ids[2])
            query("delete own row", "DELETE FROM " + table + " WHERE id=%s", (deleted,))
            check("deleted native row not returned", deleted not in ann("deleted row ANN", base[256:384]))
            inserted, final_id = int(max(ids)) + 17, -int(max(ids)) - 17
            query("insert new increment row", "INSERT INTO " + table + " VALUES(%s,2,%s)", (inserted, vector_text(base[256:384])))
            check("new increment row returned", ann("new increment ANN", base[256:384], limit=1) == (inserted,))
            query("change own primary key", "UPDATE " + table + " SET id=%s WHERE id=%s", (final_id, inserted))
            check("primary key change retains correct vector identity", ann("PK changed increment ANN", base[256:384], limit=1) == (final_id,))
            report["dml_state"] = {"updated_id": original, "deleted_id": deleted, "inserted_final_id": final_id}
            report["retained_index"] = last_index
            report["full_three_build_rounds"] = rounds == 3 and not failures
            if rounds == 3 and not failures:
                times = sorted(row["ddl_through_first_query_seconds"] for row in report["build_rounds"])
                report["whole_sql_median_seconds"] = times[1]
            report["status"] = "FAILED" if failures else "PASSED_FUNCTIONAL_REOPEN_PENDING"
        process_identity(args, identity)
        if {name: digest(root / name) for name in hashes} != hashes:
            raise ValueError("raw inputs changed during SQL verification")
    except Exception as error:
        report.update(status="FAILED", error=f"{type(error).__name__}: {error}", traceback=traceback.format_exc())
        if connection:
            try:
                connection.rollback()
            except Exception:
                pass
    finally:
        if connection:
            connection.close()
        report["elapsed_seconds"] = time.monotonic() - started
        save(); journal.close()
    print(json.dumps({"status": report["status"], "report": str(output / "report.json"), "schema": report.get("schema")}, ensure_ascii=False))
    return 0 if report["status"].startswith("PASSED_") else 1


if __name__ == "__main__":
    raise SystemExit(main())
