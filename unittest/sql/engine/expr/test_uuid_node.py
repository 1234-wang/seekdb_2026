#!/usr/bin/env python3
# Copyright (c) 2025 OceanBase.
# Licensed under the Apache License, Version 2.0.
"""Focused Linux UUID-node regression without building seekdb.

Run on Linux/WSL:
  python3 unittest/sql/engine/expr/test_uuid_node.py
  python3 unittest/sql/engine/expr/test_uuid_node.py --revision 5bce9e0 --mode baseline-loopback

Compiles the actual ObUUIDNode methods and declaration from the selected source.
Only project error constants and logging/memory macros are shimmed. The driver
uses linker syscall wrapping for deterministic failures, plus real host and
isolated loopback-only network namespace checks. This does NOT compile the full
expression translation unit, link seekdb, or prove server/SQL startup readiness.
No source files, namespace settings outside the child, or build caches are changed.
"""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="Read production source from this git revision")
    parser.add_argument("--mode", choices=("all", "mock", "real", "loopback", "baseline-loopback"), default="all")
    parser.add_argument("--sanitize", action="store_true", help="Enable ASan and UBSan")
    parser.add_argument("--emit-include", type=Path,
                        help="Write the extracted production code for the CMake test target, then exit")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("run under Linux or WSL")
    root = Path(__file__).resolve().parents[4]

    def source(relative):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{relative}"], cwd=root, text=True
            )
        return (root / relative).read_text(encoding="utf-8")

    cpp = source("src/sql/engine/expr/ob_expr_uuid.cpp")
    header = source("src/sql/engine/expr/ob_expr_uuid.h")
    errors = source("src/oblib/lib/ob_errno.h")
    declaration = re.search(r"struct ObUUIDNode\s*\{.*?\n\};", header, re.S)
    if not declaration:
        raise RuntimeError("ObUUIDNode declaration not found; update the isolated test runner")
    methods = cpp[cpp.index("ObUUIDNode::ObUUIDNode()"):cpp.index("int ObUUIDTime::get_time(")]
    platform_headers = cpp[cpp.index("#ifdef _WIN32"):cpp.index('#include "sql/engine/expr/ob_expr_uuid.h"')]
    constants = []
    for name in ("OB_SUCCESS", "OB_ERR_UNEXPECTED"):
        match = re.search(rf"\b{ name }\s*=\s*(-?\d+)\s*;", errors)
        if not match:
            raise RuntimeError(f"could not read {name} from ob_errno.h")
        constants.append(f"constexpr int {name} = {match.group(1)};")
    isolated = platform_headers + """
#define MEMSET std::memset
#define MEMCPY std::memcpy
#define MEMCMP std::memcmp
#define OB_SUCC(value) ((value) == 0)
#define OB_FAIL(value) ((ret = (value)) != 0)
#define OB_ISNULL(value) ((value) == nullptr)
#define K(value) uuid_node_test::field(#value, value)
#define LOG_ERROR(...) uuid_node_test::log(__VA_ARGS__)
#define LOG_WARN(...) uuid_node_test::log(__VA_ARGS__)
#define LOG_INFO(...) uuid_node_test::log(__VA_ARGS__)
namespace oceanbase { namespace sql {
""" + "\n".join(constants) + "\n" + declaration.group(0) + "\n" + methods + "\n}}\n"
    if args.emit_include:
        args.emit_include.parent.mkdir(parents=True, exist_ok=True)
        args.emit_include.write_text(isolated, encoding="utf-8")
        return
    with tempfile.TemporaryDirectory(prefix="seekdb-uuid-node-") as directory:
        directory = Path(directory)
        (directory / "uuid_node_under_test.inc").write_text(isolated, encoding="utf-8")
        binary = directory / "uuid_node_test"
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-g", "-O1",
                   "-DUUID_NODE_STANDALONE_TEST", "-I", str(directory),
                   str(Path(__file__).with_suffix(".cpp")), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command += [f"-Wl,--wrap={name}" for name in ("socket", "ioctl", "open", "read", "__read_chk", "close")]
        subprocess.run(command, check=True)
        modes = ("mock", "real", "loopback") if args.mode == "all" else (args.mode,)
        for mode in modes:
            run = [str(binary), mode]
            if mode in ("loopback", "baseline-loopback"):
                run = ["unshare", "--user", "--map-root-user", "--net", "--"] + run
            subprocess.run(run, check=True)


if __name__ == "__main__":
    main()
