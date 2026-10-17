"""Validation and grouping helpers for Observer's native composition root."""

load(
    "//bazel:module_sources.bzl",
    _singleton_unity_groups = "singleton_unity_groups",
    _source_owners = "source_owners",
    _validate_source_tree = "validate_source_tree",
)


def observer_singleton_unity_groups(sources, name_prefix):
    groups = _singleton_unity_groups(sources, name_prefix, "src/observer", "c++")
    return groups

def observer_validate_source_inventory(
        unity_groups,
        standalone_source_sets,
        checked_in_sources,
        ignored_sources,
        generated_sources,
):
    owners = _source_owners("src/observer", unity_groups, [source for sources in standalone_source_sets for source in sources], language = "c++")
    _validate_source_tree("src/observer", owners, checked_in_sources, ignored_sources, generated_sources)

def observer_validate_header_inventory(private_headers, checked_in_headers):
    owned = {path: True for path in private_headers}
    if len(owned) != len(private_headers):
        fail("duplicate Observer private header")

    checked_in = {path: True for path in checked_in_headers}
    missing = sorted([path for path in checked_in if path not in owned])
    stale = sorted([path for path in owned if path not in checked_in])
    if missing or stale:
        fail(
            "Observer header ownership differs from the tree: missing=%s stale=%s" %
            (missing, stale),
        )
