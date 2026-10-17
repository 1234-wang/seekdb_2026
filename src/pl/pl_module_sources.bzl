"""Validation and grouping helpers for PL's native source module."""

load(
    "//bazel:module_sources.bzl",
    _singleton_unity_groups = "singleton_unity_groups",
    _source_owners = "source_owners",
    _validate_source_tree = "validate_source_tree",
)


def pl_singleton_unity_groups(sources, name_prefix, language):
    for source in sources:
        if source.kind not in ("source", "generated"):
            fail("unsupported PL input: %s (%s)" % (source.path, source.kind))
    groups = _singleton_unity_groups(
        [source for source in sources if source.language == language],
        name_prefix, "src/pl", language, source_kinds = ["source", "generated"],
    )
    if not groups:
        fail("no %s standalone PL sources" % language)
    return groups

def pl_validate_source_inventory(unity_groups, standalone_sources, checked_in_sources):
    for group in unity_groups:
        if group.language != "c++":
            fail("PL release Unity group is not C++: %s" % group.name)
    owners = _source_owners("src/pl", unity_groups, standalone_sources, source_kinds = ["source", "generated"])
    _validate_source_tree("src/pl", owners, checked_in_sources, generated_sources = [source.path for source in standalone_sources if source.kind == "generated"])
