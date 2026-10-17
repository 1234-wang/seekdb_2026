#!/usr/bin/env python3
"""Generate exact OBLib header-owner dependencies for Bazel Unity groups.

Run this after Bazel has analyzed or built the semantic Unity target.  The
generated Unity sources retain the checked-in member list, which this tool
walks through the repository include graph until it reaches OBLib's public
header seam.
"""

from pathlib import Path
import argparse
import re
import subprocess
import sys

from oblib_dependencies import (
    INCLUDE_RE, SourceIndex, component_closures, load_owners, oblib_header, read,
)


def _all_group_deps(unity_sources, target_prefix, repo, owner_by_header, index):
    group_roots = {}
    pending = []
    prefix = target_prefix + "_"
    for unity_source in unity_sources:
        name = unity_source.name
        suffix = ".unity.cpp" if name.endswith(".unity.cpp") else ".unity.c"
        group = name[len(prefix) : -len(suffix)]
        roots = []
        for include in INCLUDE_RE.findall(read(unity_source)):
            resolved = index.resolve(unity_source, include)
            if resolved is None:
                raise ValueError(
                    "%s: cannot resolve Unity member %s"
                    % (unity_source, include)
                )
            roots.append(resolved)
        group_roots[group] = roots
        pending.extend(roots)

    graph = {}
    direct_owners = {}
    visited = set()
    while pending:
        path = pending.pop()
        if path in visited:
            continue
        visited.add(path)
        header = oblib_header(path, repo)
        if header is not None:
            continue
        dependencies = set()
        owners = set()
        for include in INCLUDE_RE.findall(read(path)):
            dependency = index.resolve(path, include)
            if dependency is None:
                continue
            dependency_header = oblib_header(dependency, repo)
            if dependency_header is not None:
                owner = owner_by_header.get(dependency_header)
                if owner is None:
                    raise ValueError(
                        "%s reaches private OBLib header %s"
                        % (path, dependency_header)
                    )
                owners.add(owner)
            else:
                dependencies.add(dependency)
                pending.append(dependency)
        graph[path] = dependencies
        direct_owners[path] = owners

    component_by_path, closures = component_closures(graph, direct_owners)

    result = {}
    for group, roots in group_roots.items():
        owners = set()
        for root in roots:
            header = oblib_header(root, repo)
            if header is not None:
                owner = owner_by_header.get(header)
                if owner is None:
                    raise ValueError(
                        "%s reaches private OBLib header %s" % (group, header)
                    )
                owners.add(owner)
            else:
                owners.update(closures[component_by_path[root]])
        result[group] = sorted(owners)
    return result


def _render(symbol, group_deps):
    private_symbol = "_" + symbol.upper()
    lines = [
        '"""Generated exact OBLib dependencies for semantic Unity groups."""',
        "",
        "%s = {" % private_symbol,
    ]
    for group in sorted(group_deps):
        lines.append('    "%s": [' % group)
        for dependency in group_deps[group]:
            lines.append('        "%s",' % dependency)
        lines.append("    ],")
    lines.extend(
        [
            "}",
            "",
            "def %s(groups, base = {}):" % symbol,
            "    actual = {group.name: True for group in groups}",
            "    unknown = sorted([",
            "        name",
            "        for name in %s" % private_symbol,
            "        if name not in actual",
            "    ])",
            "    missing = sorted([",
            "        name",
            "        for name in actual",
            "        if name not in %s" % private_symbol,
            "    ])",
            "    if unknown or missing:",
            '        fail("OBLib Unity dependency map differs from inventory: unknown=%s missing=%s" % (unknown, missing))',
            "    return {",
            "        name: base.get(name, []) + %s[name]" % private_symbol,
            "        for name in actual",
            "    }",
            "",
        ]
    )
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unity-dir", type=Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--target-prefix", required=True)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--groups-file",
        type=Path,
        help="take the current group names from an existing generated map",
    )
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    repo = Path(__file__).resolve().parents[2]
    unity_dir = arguments.unity_dir.resolve()
    if arguments.groups_file:
        current_groups = set(
            re.findall(
                r'^    "([^"]+)": \[$',
                read(arguments.groups_file),
                re.MULTILINE,
            )
        )
    else:
        package = arguments.target.split(":", 1)[0]
        query = 'filter("^%s:_%s_.*_generated_unity$", deps(%s))' % (
            package,
            arguments.target_prefix,
            arguments.target,
        )
        labels = subprocess.check_output(
            [str(repo / "bazel.py"), "query", query, "--output=label"],
            cwd=str(repo),
            universal_newlines=True,
        )
        label_prefix = package + ":_" + arguments.target_prefix + "_"
        current_groups = {
            label[len(label_prefix) : -len("_generated_unity")]
            for label in labels.splitlines()
            if label.startswith(label_prefix)
            and label.endswith("_generated_unity")
        }

    patterns = (
        arguments.target_prefix + "_*.unity.cpp",
        arguments.target_prefix + "_*.unity.c",
    )
    unity_sources = sorted(
        {
            path
            for pattern in patterns
            for path in unity_dir.glob(pattern)
            if path.is_file()
        }
    )
    unity_sources = [
        path
        for path in unity_sources
        if any(
            path.name
            == arguments.target_prefix
            + "_"
            + group
            + suffix
            for group in current_groups
            for suffix in (".unity.cpp", ".unity.c")
        )
    ]
    if not unity_sources:
        print(
            "no generated Unity sources found under %s for %s"
            % (unity_dir, arguments.target_prefix),
            file=sys.stderr,
        )
        return 1

    owner_by_header = load_owners(repo)
    index = SourceIndex(repo, include_generated=True)
    group_deps = _all_group_deps(
        unity_sources,
        arguments.target_prefix,
        repo,
        owner_by_header,
        index,
    )
    missing_groups = sorted(current_groups - set(group_deps))
    if missing_groups:
        print(
            "generated Unity source is missing for groups: %s"
            % ", ".join(missing_groups),
            file=sys.stderr,
        )
        return 1
    rendered = _render(arguments.symbol, group_deps)
    if arguments.check:
        if not arguments.output.is_file() or read(arguments.output) != rendered:
            print(
                "OBLib Unity dependency map is stale: %s" % arguments.output,
                file=sys.stderr,
            )
            return 1
    else:
        arguments.output.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
