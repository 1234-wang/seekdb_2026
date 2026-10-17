#!/usr/bin/env python3
"""Verify that SQL's native inventory owns every checked-in source exactly once."""

import sys
from collections import Counter
from pathlib import Path
from typing import List

from inventory import COMPILE_SUFFIXES, load_inventory, source_paths, tracked_files


INVENTORY_NAMES = (
    "SQL_UNITY_GROUPS",
    "SQL_SIMD_UNITY_GROUPS",
    "SQL_STANDALONE_SOURCES",
    "SQL_EXTRA_SOURCES",
    "SQL_PARSER_SOURCES",
)
SEPARATELY_OWNED_SOURCES = {
    # Upstream checked-in prototype duplicates ob_dtl_mem_manager and is not
    # referenced by the production DTL runtime.
    "src/sql/dtl/ob_dtl_memory_manager.cpp",
}


def check(repo: Path) -> List[str]:
    inventory = repo / "src/sql/sql_source_inventory.bzl"
    sections = {
        name: source_paths(records)
        for name, records in load_inventory(inventory, INVENTORY_NAMES).items()
    }
    owned = [path for paths in sections.values() for path in paths]
    duplicates = sorted(path for path, count in Counter(owned).items() if count > 1)

    tracked = {
        path.relative_to(repo).as_posix()
        for path in tracked_files(repo, repo / "src/sql", COMPILE_SUFFIXES).values()
    }
    separate = tracked & SEPARATELY_OWNED_SOURCES
    expected_inventory = tracked - separate
    actual_inventory = set(owned)

    errors = []
    if duplicates:
        errors.append(f"duplicate inventory sources: {duplicates}")
    missing = sorted(expected_inventory - actual_inventory)
    stale = sorted(actual_inventory - expected_inventory)
    if missing:
        errors.append(f"unowned checked-in SQL sources: {missing}")
    if stale:
        errors.append(f"stale/non-production SQL inventory sources: {stale}")

    expected_counts = {
        "SQL_UNITY_GROUPS": 1107,
        "SQL_SIMD_UNITY_GROUPS": 3,
        "SQL_STANDALONE_SOURCES": 5,
        "SQL_EXTRA_SOURCES": 21,
        "SQL_PARSER_SOURCES": 15,
    }
    for name, expected in expected_counts.items():
        actual = len(sections[name])
        if actual != expected:
            errors.append(f"{name} has {actual} sources, expected {expected}")
    missing_separate = sorted(SEPARATELY_OWNED_SOURCES - tracked)
    if missing_separate:
        errors.append(f"stale separately-owned SQL sources: {missing_separate}")
    return errors


def main() -> int:
    repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    errors = check(repo)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("sql source ownership: inventory matches workspace sources")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
