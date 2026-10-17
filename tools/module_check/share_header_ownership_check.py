#!/usr/bin/env python3
"""Verify Share's native public/closure/private header ownership."""

import sys
from pathlib import Path

from inventory import (
    HEADER_SUFFIXES, SOURCE_SUFFIXES, INCLUDE_PATTERN,
    load_inventory, header_ownership, files_under,
)


INVENTORY_NAMES = {
    "SHARE_PUBLIC_HEADER_ROOTS",
    "SHARE_INTERFACE_CLOSURE_HEADERS",
    "SHARE_PRIVATE_HEADERS",
}
GENERATED_SOURCE_TREE_HEADERS = {
    "inner_table/ob_inner_table_schema.h",
    "inner_table/ob_inner_table_schema_constants.h",
    "inner_table/ob_inner_table_schema_misc.ipp",
}
DECLARED_PUBLIC_HEADER_ROOTS = {
    "cache/ob_cache_name_define.h",
    "compaction/ob_ckm_error_tablet_info.h",
    "ob_ddl_task_serialize_field.h",
    "ob_id_generator.h",
    "schema/ob_schema_publish_signal.h",
    "table/ob_ttl_schedule.h",
}


def _derive_ownership(repo, share_root, headers):
    public, closure, private, consumers = header_ownership(
        repo, share_root, headers, files_under(repo / "src", SOURCE_SUFFIXES).values(),
        declared_public=DECLARED_PUBLIC_HEADER_ROOTS,
    )
    return {
        "SHARE_PUBLIC_HEADER_ROOTS": public,
        "SHARE_INTERFACE_CLOSURE_HEADERS": closure,
        "SHARE_PRIVATE_HEADERS": private,
    }, consumers


def main():
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    share_root = repo / "src/share"
    inventory_path = share_root / "share_header_inventory.bzl"
    inventory = {
        name: set(values)
        for name, values in load_inventory(inventory_path, INVENTORY_NAMES).items()
    }
    headers = files_under(share_root, HEADER_SUFFIXES, GENERATED_SOURCE_TREE_HEADERS)
    derived, consumers = _derive_ownership(repo, share_root, headers)

    errors = []
    all_owned = set()
    for name in sorted(INVENTORY_NAMES):
        values = inventory[name]
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

    if errors:
        for error in errors:
            print("[FAIL] " + error, file=sys.stderr)
        return 1

    print(
        "share header ownership: %d tracked, %d public roots, "
        "%d interface closure, %d private"
        % (
            len(headers),
            len(derived["SHARE_PUBLIC_HEADER_ROOTS"]),
            len(derived["SHARE_INTERFACE_CLOSURE_HEADERS"]),
            len(derived["SHARE_PRIVATE_HEADERS"]),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
