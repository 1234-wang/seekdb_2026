#!/usr/bin/env python3
"""Generate exact OBLib owner dependencies for non-partitioned compile targets.

Large semantic Unity modules use per-group dependency maps.  This generator
covers the remaining source-bearing cc_library actions (small semantic
libraries, generated single-Unity targets, and compile probes).  It walks each
translation unit's repository include closure until it reaches OBLib, then
records the fine-grained owners of those directly reached OBLib headers.
"""

from pathlib import Path
import argparse
import subprocess
import sys
import xml.etree.ElementTree as ET

from oblib_dependencies import (
    INCLUDE_RE, SourceIndex, component_closures, load_owners, oblib_header, read,
)


COMPILE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
PARTITIONED_TARGET_PREFIXES = (
    "//src/observer:_observer_main_",
    "//src/observer:_observer_retrieval_composition_",
    "//src/observer:_observer_runtime_",
    "//src/pl:_pl_parser_c_",
    "//src/pl:_pl_runtime_",
    "//src/rootserver:_rootserver_runtime_",
    "//src/share:_share_runtime_",
    "//src/sql:_sql_runtime_",
    "//src/storage:_storage_runtime_",
)


def _query_xml(repo, expression):
    return ET.fromstring(
        subprocess.check_output(
            [
                str(repo / "bazel.py"),
                "query",
                expression,
                "--output=xml",
            ],
            cwd=str(repo),
        )
    )


def _list_labels(rule, attribute):
    value = rule.find("list[@name='%s']" % attribute)
    if value is None:
        return []
    return [label.attrib["value"] for label in value.findall("label")]


def _generated_unity_members(repo):
    rules = _query_xml(
        repo,
        'kind("_emit_unity_source rule", //src/...:*)',
    )
    result = {}
    for rule in rules.findall("rule"):
        result[rule.attrib["name"]] = _list_labels(rule, "srcs")
    return result


def _label_sources(label, repo, generated_unity_members, resolving=None):
    resolving = set() if resolving is None else resolving
    if label in resolving:
        return []
    if label in generated_unity_members:
        result = []
        for member in generated_unity_members[label]:
            result.extend(
                _label_sources(
                    member,
                    repo,
                    generated_unity_members,
                    resolving | {label},
                )
            )
        return result
    if not label.startswith("//") or ":" not in label:
        return []
    package, name = label[2:].split(":", 1)
    path = repo / package / name
    return [path.resolve()] if path.is_file() else []


def _build_registry(repo):
    owners = load_owners(repo)
    generated_unity_members = _generated_unity_members(repo)
    cc_rules = _query_xml(
        repo,
        'kind("cc_library rule", //src/...:*) except //src/oblib/...:*',
    )
    roots_by_target = {}
    all_roots = set()
    for rule in cc_rules.findall("rule"):
        roots = []
        for label in _list_labels(rule, "srcs"):
            roots.extend(
                _label_sources(label, repo, generated_unity_members)
            )
        roots = sorted(
            {
                path
                for path in roots
                if path.suffix.lower() in COMPILE_SUFFIXES
            }
        )
        if roots:
            target = rule.attrib["name"]
            if (
                target.endswith("_objects")
                and any(
                    target.startswith(prefix)
                    for prefix in PARTITIONED_TARGET_PREFIXES
                )
            ):
                continue
            roots_by_target[target] = roots
            all_roots.update(roots)

    index = SourceIndex(repo)
    graph = {}
    direct_owners = {}
    pending = list(all_roots)
    while pending:
        path = pending.pop()
        if path in graph:
            continue
        dependencies = set()
        path_owners = set()
        for include in INCLUDE_RE.findall(read(path)):
            dependency = index.resolve(path, include)
            if dependency is None:
                continue
            header = oblib_header(dependency, repo)
            if header is not None:
                owner = owners.get(header)
                if owner is None:
                    raise ValueError(
                        "%s reaches private OBLib header %s"
                        % (path, header)
                    )
                path_owners.add(owner)
            else:
                dependencies.add(dependency)
                pending.append(dependency)
        graph[path] = dependencies
        direct_owners[path] = path_owners

    component_by_path, closures = component_closures(
        graph,
        direct_owners,
    )
    registry = {}
    for target, roots in sorted(roots_by_target.items()):
        if target.startswith("//src/oblib:"):
            continue
        required = {
            owner
            for root in roots
            for owner in closures[component_by_path[root]]
        }
        if required:
            registry[target] = sorted(required)
    return registry


def _render(registry):
    lines = [
        '"""Generated exact OBLib dependencies for compile actions."""',
        "",
        "_OBLIB_COMPILE_DEPS = {",
    ]
    for target, dependencies in registry.items():
        lines.append('    "%s": [' % target)
        for dependency in dependencies:
            lines.append('        "%s",' % dependency)
        lines.append("    ],")
    lines.extend(
        [
            "}",
            "",
            "def oblib_compile_deps(name):",
            '    label = "//%s:%s" % (native.package_name(), name)',
            "    return _OBLIB_COMPILE_DEPS.get(label, [])",
            "",
        ]
    )
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    output = arguments.output or repo / "bazel" / "oblib_compile_deps.bzl"
    rendered = _render(_build_registry(repo))
    if arguments.check:
        if not output.is_file() or read(output) != rendered:
            print(
                "OBLib compile dependency registry is stale: %s" % output,
                file=sys.stderr,
            )
            return 1
        return 0
    output.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
