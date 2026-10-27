#!/usr/bin/env python3
"""Compile the production UUID node initializer with fault-injected OS calls.

Run on Linux with Python 3 and a C++ compiler. This focused test does not build
or link the SQL engine; only logging/error macros and OS calls are substituted.
"""
import os
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[4]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=root / "src/sql/engine/expr/ob_expr_uuid.cpp")
    source = parser.parse_args().source.read_text()
    start = source.find("#if defined(__linux__)\n// Stage")
    if start < 0:
        start = source.index("ObUUIDNode::ObUUIDNode()")
    end = source.index("int ObUUIDTime::get_time", start)
    harness = Path(__file__).with_name("uuid_node_harness.inc").read_text()
    prefix, suffix = harness.split("// PRODUCTION_UUID_NODE_INITIALIZER\n")
    with tempfile.TemporaryDirectory(prefix="seekdb-uuid-node-") as directory:
        cpp = Path(directory) / "test.cpp"
        executable = Path(directory) / "test_uuid_node"
        cpp.write_text(prefix + source[start:end] + suffix)
        for defines in ([], ["-D__ANDROID__"]):
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall",
                            "-Wextra", *defines, str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
