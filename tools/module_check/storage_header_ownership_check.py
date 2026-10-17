#!/usr/bin/env python3
"""Verify Storage's native public/closure/private header ownership."""

import sys
from pathlib import Path

from inventory import (
    HEADER_SUFFIXES, SOURCE_SUFFIXES, INCLUDE_PATTERN,
    load_inventory, header_ownership, files_under,
)


EXPECTED_COUNTS = {
    "STORAGE_PUBLIC_HEADER_ROOTS": 131,
    "STORAGE_INTERFACE_CLOSURE_HEADERS": 469,
    "STORAGE_PRIVATE_HEADERS": 194,
    "STORAGE_SEMANTIC_HEADERS": 6,
}
EXPECTED_SEMANTIC_HEADERS = {
    "blocksstable/ob_io_bench_controller.h",
    "ls/ob_i_ls_runtime_adapter.h",
    "memtable/ob_i_multi_source_data_unit.h",
    "meta_mem/ob_i_storage_meta_obj.h",
    "ob_tablet_autoincrement_service.h",
    "tablet/ob_tablet_autoincrement_state.h",
}


def _derive_ownership(repo, storage_root, headers):
    public, closure, private, consumers = header_ownership(
        repo, storage_root, headers, files_under(repo / "src", SOURCE_SUFFIXES).values(),
    )
    special = set(EXPECTED_SEMANTIC_HEADERS)
    return {
        "STORAGE_PUBLIC_HEADER_ROOTS": public - special,
        "STORAGE_INTERFACE_CLOSURE_HEADERS": closure - special,
        "STORAGE_PRIVATE_HEADERS": private - special,
        "STORAGE_SEMANTIC_HEADERS": special,
    }, consumers


def _direct_sql_includes(storage_root):
    violations = []
    for source in storage_root.rglob("*"):
        if not source.is_file() or source.suffix not in SOURCE_SUFFIXES:
            continue
        for include in INCLUDE_PATTERN.findall(source.read_text(errors="ignore")):
            if include.startswith("sql/") or include.startswith("src/sql/"):
                violations.append(
                    "%s -> %s"
                    % (source.relative_to(storage_root).as_posix(), include)
                )
    return sorted(violations)


def main():
    repo = (
        Path(sys.argv[1]).resolve()
        if len(sys.argv) > 1
        else Path(__file__).resolve().parents[2]
    )
    storage_root = repo / "src/storage"
    inventory_path = storage_root / "storage_header_inventory.bzl"
    inventory = {
        name: set(values)
        for name, values in load_inventory(inventory_path, EXPECTED_COUNTS).items()
    }
    headers = files_under(storage_root, HEADER_SUFFIXES)
    derived, consumers = _derive_ownership(repo, storage_root, headers)

    errors = []
    all_owned = set()
    for name, expected_count in EXPECTED_COUNTS.items():
        values = inventory[name]
        if len(values) != expected_count:
            errors.append(
                "%s has %d entries, expected %d"
                % (name, len(values), expected_count)
            )
        duplicate_owners = sorted(all_owned & values)
        if duplicate_owners:
            errors.append("headers with multiple owners: %s" % duplicate_owners)
        all_owned.update(values)

        missing = sorted(derived[name] - values)
        stale = sorted(values - derived[name])
        if missing:
            detail = []
            for header in missing[:10]:
                sites = sorted(consumers.get(header, ()))[:3]
                detail.append("%s <- %s" % (header, sites))
            errors.append("%s missing current headers: %s" % (name, detail))
        if stale:
            errors.append("%s contains stale headers: %s" % (name, stale[:10]))

    tracked_headers = set(headers)
    if all_owned != tracked_headers:
        errors.append(
            "ownership is not complete: missing=%s extra=%s"
            % (
                sorted(tracked_headers - all_owned)[:10],
                sorted(all_owned - tracked_headers)[:10],
            )
        )

    sql_includes = _direct_sql_includes(storage_root)
    if sql_includes:
        errors.append("Storage directly includes SQL headers: %s" % sql_includes[:10])

    if errors:
        for error in errors:
            print("[FAIL] " + error, file=sys.stderr)
        return 1

    print(
        "storage header ownership: %d tracked, %d public roots, "
        "%d interface closure, %d private, %d semantic"
        % (
            len(headers),
            len(derived["STORAGE_PUBLIC_HEADER_ROOTS"]),
            len(derived["STORAGE_INTERFACE_CLOSURE_HEADERS"]),
            len(derived["STORAGE_PRIVATE_HEADERS"]),
            len(derived["STORAGE_SEMANTIC_HEADERS"]),
        )
    )
    print("storage SQL include check: direct includes 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
