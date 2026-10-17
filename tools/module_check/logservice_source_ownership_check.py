#!/usr/bin/env python3
"""Verify that Logservice's Unity inventory owns every checked-in source once."""

import sys
from collections import Counter
from pathlib import Path

from inventory import COMPILE_SUFFIXES, load_inventory, source_paths, tracked_files


EXPECTED_GROUPS = {
    "ob_logservice_common_0",
    "ob_logservice_common_mixed_0",
    "ob_logservice_common_util_0",
    "ob_logservice_palf_0",
    "ob_logservice_palf_1",
}
EXPECTED_SOURCE_COUNT = 58


def check(repo):
    records = load_inventory(
        repo / "src/logservice/logservice_build_defs.bzl", ["LOGSERVICE_UNITY_GROUPS"],
    )["LOGSERVICE_UNITY_GROUPS"]
    groups = {group["name"] for group in records}
    owned = source_paths(records)
    tracked = {
        path.relative_to(repo).as_posix()
        for path in tracked_files(repo, repo / "src/logservice", COMPILE_SUFFIXES).values()
    }
    owned_set = set(owned)
    errors = []

    if groups != EXPECTED_GROUPS:
        errors.append(
            "Unity groups differ: missing=%s extra=%s"
            % (sorted(EXPECTED_GROUPS - groups), sorted(groups - EXPECTED_GROUPS))
        )
    duplicates = sorted(path for path, count in Counter(owned).items() if count > 1)
    if duplicates:
        errors.append("duplicate inventory sources: %s" % duplicates)
    missing = sorted(tracked - owned_set)
    stale = sorted(owned_set - tracked)
    if missing:
        errors.append("unowned checked-in Logservice sources: %s" % missing)
    if stale:
        errors.append("stale Logservice inventory sources: %s" % stale)
    if len(owned) != EXPECTED_SOURCE_COUNT:
        errors.append(
            "Logservice inventory has %d sources, expected %d"
            % (len(owned), EXPECTED_SOURCE_COUNT)
        )
    return errors


def main():
    repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    errors = check(repo)
    if errors:
        for error in errors:
            print("[FAIL] " + error, file=sys.stderr)
        return 1
    print(
        "logservice source ownership: %d sources in %d Unity groups"
        % (EXPECTED_SOURCE_COUNT, len(EXPECTED_GROUPS))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
