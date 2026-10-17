"""Source grouping and ownership shared by seekdb's native modules."""

def _package_path(package, path):
    prefix = package + "/"
    if not path.startswith(prefix):
        fail("source outside %s: %s" % (package, path))
    return path[len(prefix):]

def select_unity_groups(groups, names, language = "c++"):
    """Selects named groups in inventory order, checking the complete selection."""
    requested = {name: False for name in names}
    selected = []
    for group in groups:
        if (language == None or group.language == language) and group.name in requested:
            if requested[group.name]:
                fail("duplicate Unity group: %s" % group.name)
            requested[group.name] = True
            selected.append(group)
    missing = sorted([name for name, found in requested.items() if not found])
    if missing:
        fail("missing Unity groups: %s" % missing)
    return selected

def singleton_unity_groups(sources, name_prefix, package, language = "c++", source_kinds = ["source"]):
    """Keeps each standalone source in its own ordered compile action."""
    groups = []
    for index, source in enumerate(sources):
        if source.kind not in source_kinds or source.language != language:
            fail("unsupported %s source: %s (%s, %s)" % (package, source.path, source.kind, source.language))
        _package_path(package, source.path)
        groups.append(struct(
            name = "%s_%d" % (name_prefix, index),
            language = language,
            srcs = [source.path],
        ))
    return groups

def source_owners(package, groups, sources, language = None, source_kinds = ["source"]):
    """Checks group/source ownership and returns workspace-path to owner mappings."""
    owners = {}
    group_names = {}
    for group in groups:
        key = group.language + ":" + group.name
        if key in group_names:
            fail("duplicate %s Unity group: %s" % (package, key))
        group_names[key] = True
        if language != None and group.language != language:
            fail("%s expects %s groups, got %s" % (package, language, group.language))
        for path in group.srcs:
            _package_path(package, path)
            if path in owners:
                fail("source %s is owned by both %s and %s" % (path, owners[path], group.name))
            owners[path] = group.name
    for source in sources:
        if source.kind not in source_kinds or (language != None and source.language != language):
            fail("unsupported %s source: %s (%s, %s)" % (package, source.path, source.kind, source.language))
        _package_path(package, source.path)
        if source.path in owners:
            fail("duplicate standalone source: %s" % source.path)
        owners[source.path] = "standalone"
    return owners

def validate_source_tree(package, owners, checked_in_sources, ignored_sources = [], generated_sources = []):
    """Matches ownership against the tree, with explicit ignored/generated inputs."""
    owned = {_package_path(package, path): True for path in owners}
    ignored = {path: True for path in ignored_sources}
    overlap = sorted([path for path in ignored if path in owned])
    if overlap:
        fail("ignored %s sources are also owned: %s" % (package, overlap))
    generated = {_package_path(package, path): True for path in generated_sources}
    missing_generated = sorted([path for path in generated if path not in owned])
    if missing_generated:
        fail("generated %s sources are not owned: %s" % (package, missing_generated))
    checked_in = {path: True for path in checked_in_sources}
    missing = sorted([path for path in checked_in if path not in owned and path not in ignored])
    stale = sorted([path for path in owned if path not in checked_in and path not in generated])
    stale_ignored = sorted([path for path in ignored if path not in checked_in])
    if missing or stale or stale_ignored:
        fail("%s source ownership differs from the tree: missing=%s stale=%s stale_ignored=%s" %
             (package, missing, stale, stale_ignored))
