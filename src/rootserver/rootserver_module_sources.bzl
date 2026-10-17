"""Validation and grouping helpers for Rootserver's native source module."""

load(
    "//bazel:module_sources.bzl",
    _singleton_unity_groups = "singleton_unity_groups",
    _source_owners = "source_owners",
    _validate_source_tree = "validate_source_tree",
)


def rootserver_singleton_unity_groups(sources, name_prefix):
    groups = _singleton_unity_groups(sources, name_prefix, "src/rootserver", "c++")
    return groups

def rootserver_validate_source_inventory(
        unity_groups,
        standalone_sources,
        checked_in_sources,
        ignored_sources,
):
    owners = _source_owners("src/rootserver", unity_groups, standalone_sources, language = "c++")
    _validate_source_tree("src/rootserver", owners, checked_in_sources, ignored_sources)
