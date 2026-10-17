"""Helpers for declaring Share's native semantic source graph."""

load(
    "//bazel:module_sources.bzl",
    _singleton_unity_groups = "singleton_unity_groups",
    _source_owners = "source_owners",
    _select_unity_groups = "select_unity_groups",
)

_SHARE_SOURCE_PREFIX = "src/share/"


share_select_unity_groups = _select_unity_groups

def share_package_sources(sources, language = "c++"):
    """Converts checked-in standalone source records to package paths."""

    result = []
    for source in sources:
        if source.kind != "source":
            fail("non-source Share input: %s (%s)" % (source.path, source.kind))
        if source.language == language:
            if not source.path.startswith(_SHARE_SOURCE_PREFIX):
                fail("source outside src/share: %s" % source.path)
            result.append(source.path[len(_SHARE_SOURCE_PREFIX):])
    if not result:
        fail("no %s standalone Share sources" % language)
    return result

def share_select_unity_sources(groups, name, source_paths, language = "c++"):
    """Builds one semantic Unity group from exact checked-in Unity members."""

    requested = {path: False for path in source_paths}
    selected = []
    for group in groups:
        if group.language != language:
            continue
        for path in group.srcs:
            if path in requested:
                if requested[path]:
                    fail("duplicate Share Unity source: %s" % path)
                requested[path] = True
                selected.append(path)
    missing = sorted([path for path, found in requested.items() if not found])
    if missing:
        fail("missing %s Share Unity sources: %s" % (language, missing))
    return struct(
        name = name,
        language = language,
        srcs = selected,
    )

def share_unity_group_sources(groups):
    """Flattens semantic Unity groups into one duplicate-checked source set."""

    result = []
    seen = {}
    for group in groups:
        for source in group.srcs:
            if source in seen:
                fail(
                    "Share semantic source %s is owned by both %s and %s" %
                    (source, seen[source], group.name),
                )
            seen[source] = group.name
            result.append(source)
    return result

def share_singleton_unity_groups(sources, name_prefix, language = "c++"):
    groups = _singleton_unity_groups(sources, name_prefix, "src/share", language)
    if not groups:
        fail("no %s standalone Share sources" % language)
    return groups

def share_remaining_unity_groups(groups, excluded_sources, language = "c++"):
    """Preserves release Unity action boundaries after native extractions."""

    excluded = {source: False for source in excluded_sources}
    result = []
    for group in groups:
        if group.language != language:
            continue
        members = []
        for source in group.srcs:
            if source in excluded:
                excluded[source] = True
            else:
                members.append(source)
        if members:
            result.append(struct(
                name = group.name,
                language = group.language,
                srcs = members,
            ))
    missing = sorted([source for source, found in excluded.items() if not found])
    if missing:
        fail("unknown excluded Share sources: %s" % missing)
    return result

def share_validate_source_inventory(unity_groups, standalone_sources):
    _source_owners("src/share", unity_groups, standalone_sources)
