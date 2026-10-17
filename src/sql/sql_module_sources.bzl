"""Helpers for declaring SQL's native semantic source graph."""

load(
    "//bazel:module_sources.bzl",
    _singleton_unity_groups = "singleton_unity_groups",
    _source_owners = "source_owners",
    _select_unity_groups = "select_unity_groups",
)


SQL_OPTIMIZER_GROUP_NAMES = [
    "ob_sql_optimizer_0",
    "ob_sql_optimizer_1",
    "ob_sql_optimizer_2",
    "ob_sql_optimizer_stat_0",
    "ob_sql_optimizer_stat_1",
]

SQL_PREPARE_SOURCES = [
    "src/sql/engine/prepare/ob_deallocate_executor.cpp",
    "src/sql/engine/prepare/ob_execute_executor.cpp",
    "src/sql/engine/prepare/ob_prepare_executor.cpp",
    "src/sql/resolver/prepare/ob_deallocate_resolver.cpp",
    "src/sql/resolver/prepare/ob_deallocate_stmt.cpp",
    "src/sql/resolver/prepare/ob_execute_resolver.cpp",
    "src/sql/resolver/prepare/ob_execute_stmt.cpp",
    "src/sql/resolver/prepare/ob_prepare_resolver.cpp",
    "src/sql/resolver/prepare/ob_prepare_stmt.cpp",
]

# These generated-template families make the custom LLVM Sanity pass more than
# 3.8x slower in a representative local action. Keep them as standalone
# translation units with ENABLE_SANITY, but make the compile-cost exception
# explicit by not loading the pass.
SQL_SANITY_WITHOUT_PASS_STANDALONE_PATHS = []


def sql_headers_outside(headers, prefixes):
    """Returns root-package headers after semantic subpackage ownership."""

    return [
        header
        for header in headers
        if not any([header.startswith(prefix) for prefix in prefixes])
    ]

def sql_paths_under(paths, prefix):
    """Returns package-relative paths owned below one prefix."""

    return [
        path[len(prefix):]
        for path in paths
        if path.startswith(prefix)
    ]

def sql_headers_under(headers, prefix):
    """Returns package-relative headers owned below one semantic prefix."""

    return sql_paths_under(headers, prefix)

def sql_groups_named(groups, names):
    return _select_unity_groups(groups, names, language = None)

def sql_groups_without(groups, excluded_group_names, excluded_sources):
    """Removes separately-owned semantic groups and sources from root SQL."""

    excluded_groups = {name: False for name in excluded_group_names}
    excluded_paths = {path: False for path in excluded_sources}
    result = []
    for group in groups:
        if group.name in excluded_groups:
            excluded_groups[group.name] = True
            continue
        srcs = []
        for path in group.srcs:
            if path in excluded_paths:
                excluded_paths[path] = True
            else:
                srcs.append(path)
        if srcs:
            result.append(struct(
                name = group.name,
                language = group.language,
                srcs = srcs,
            ))

    missing_groups = sorted([
        name
        for name, found in excluded_groups.items()
        if not found
    ])
    missing_paths = sorted([
        path
        for path, found in excluded_paths.items()
        if not found
    ])
    if missing_groups or missing_paths:
        fail(
            "stale SQL semantic exclusions: groups=%s sources=%s" %
            (missing_groups, missing_paths),
        )
    return result

def sql_partition_sources(sources, selected_paths):
    """Partitions source structs while rejecting stale selected paths."""

    selected_state = {path: False for path in selected_paths}
    selected = []
    remaining = []
    for source in sources:
        if source.path in selected_state:
            selected_state[source.path] = True
            selected.append(source)
        else:
            remaining.append(source)

    missing = sorted([
        path
        for path, found in selected_state.items()
        if not found
    ])
    if missing:
        fail("unknown SQL source selection: %s" % missing)
    return struct(
        selected = selected,
        remaining = remaining,
    )

def sql_singleton_unity_groups(sources, name_prefix, language = "c++"):
    groups = _singleton_unity_groups(sources, name_prefix, "src/sql", language)
    return groups

def sql_validate_source_inventory(
        unity_groups,
        simd_unity_groups,
        standalone_sources,
        extra_sources,
        parser_sources,
        separately_owned_sources,
):
    owners = _source_owners("src/sql", unity_groups + simd_unity_groups, standalone_sources + extra_sources)
    for paths, prefix in [(parser_sources, "src/sql/parser/"), (separately_owned_sources, "src/sql/")]:
        for path in paths:
            if not path.startswith(prefix):
                fail("source outside %s: %s" % (prefix, path))
            if path in owners:
                fail("duplicate SQL source: %s" % path)
            owners[path] = "separate"
    sql_groups_named(unity_groups, SQL_OPTIMIZER_GROUP_NAMES)
    sql_groups_without(unity_groups, SQL_OPTIMIZER_GROUP_NAMES, SQL_PREPARE_SOURCES)
