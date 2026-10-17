"""Helpers for declaring Storage's native semantic source graph."""

load(
    "//bazel:module_sources.bzl",
    _singleton_unity_groups = "singleton_unity_groups",
    _source_owners = "source_owners",
    _validate_source_tree = "validate_source_tree",
)



def storage_singleton_unity_groups(sources, name_prefix, language = "c++"):
    groups = _singleton_unity_groups(sources, name_prefix, "src/storage", language)
    return groups

def storage_validate_source_inventory(
        unity_groups,
        simd_unity_groups,
        standalone_sources,
        checked_in_sources,
        separately_owned_sources,
):
    owners = _source_owners("src/storage", unity_groups + simd_unity_groups, standalone_sources)
    for path in separately_owned_sources:
        full_path = "src/storage/" + path
        if full_path in owners:
            fail("separately owned Storage source is duplicated: %s" % path)
        owners[full_path] = "separate"
    _validate_source_tree("src/storage", owners, checked_in_sources)
