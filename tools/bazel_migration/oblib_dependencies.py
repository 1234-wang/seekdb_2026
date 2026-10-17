"""Include resolution and cycle-safe OBLib ownership for dependency generators."""

from collections import defaultdict
from pathlib import Path
import re

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".def", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tcc"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


class SourceIndex:
    """Resolve includes against source files and optional generated-file aliases."""

    def __init__(self, repo, include_generated=False):
        self.repo = repo
        self.include_generated = include_generated
        self.relative = {}
        for path in (repo / "src").rglob("*"):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                self.relative[path.relative_to(repo).as_posix()] = path if include_generated else path.resolve()
        generated_root = repo / "build_bazel/bin/src"
        if include_generated and generated_root.exists():
            for path in generated_root.rglob("*"):
                if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                    self.relative["src/" + path.relative_to(generated_root).as_posix()] = path
        self.suffixes = defaultdict(list)
        self.basenames = defaultdict(list)
        for name, path in self.relative.items():
            self.basenames[path.name].append(path)
            for prefix in ("src/", "src/query/api/", "src/data_plane/api/"):
                if name.startswith(prefix):
                    self.suffixes[name[len(prefix):]].append(path)

    def _unique(self, matches):
        if len(matches) == 1:
            return matches[0]
        if self.include_generated:
            resolved = {path.resolve() for path in matches}
            if len(resolved) == 1:
                return next(iter(resolved))
        return None

    def resolve(self, source, include):
        if include.startswith("emscripten/"):
            return None
        candidates = [(source.parent / include).resolve()] if source is not None else []
        candidates.extend((self.repo / prefix / include).resolve() for prefix in (
            "", "src", "src/oblib", "src/query/api", "src/data_plane/api",
        ))
        for candidate in candidates:
            try:
                name = candidate.relative_to(self.repo).as_posix()
            except ValueError:
                continue
            if name in self.relative:
                return self.relative[name]
        match = self._unique(self.suffixes.get(include, ()))
        if match is not None:
            return match
        return self._unique(self.basenames.get(Path(include).name, ()))


def read(path):
    return path.read_text(encoding="utf-8", errors="surrogateescape")


def load_owners(repo):
    namespace = {}
    inventory = repo / "src" / "oblib" / "oblib_header_inventory.bzl"
    exec(compile(read(inventory), str(inventory), "exec"), {}, namespace)
    return {
        header: "//src/oblib" + owner
        for header, owner in namespace[
            "OBLIB_HEADER_TARGET_FOR_HEADER"
        ].items()
    }


def oblib_header(path, repo):
    try:
        relative = path.relative_to(repo / "src" / "oblib").as_posix()
    except ValueError:
        return None
    return None if relative.startswith("easy/") else relative


def component_closures(graph, direct_owners):
    indices = {}
    lowlinks = {}
    stack = []
    on_stack = set()
    components = []

    def visit(node):
        indices[node] = len(indices)
        lowlinks[node] = indices[node]
        stack.append(node)
        on_stack.add(node)
        for dependency in graph.get(node, ()):
            if dependency not in indices:
                visit(dependency)
                lowlinks[node] = min(lowlinks[node], lowlinks[dependency])
            elif dependency in on_stack:
                lowlinks[node] = min(lowlinks[node], indices[dependency])
        if lowlinks[node] == indices[node]:
            component = []
            while True:
                member = stack.pop()
                on_stack.remove(member)
                component.append(member)
                if member == node:
                    break
            components.append(component)

    for node in graph:
        if node not in indices:
            visit(node)

    component_by_path = {
        path: index
        for index, component in enumerate(components)
        for path in component
    }
    component_graph = {index: set() for index in range(len(components))}
    component_owners = {index: set() for index in range(len(components))}
    for path, dependencies in graph.items():
        owner = component_by_path[path]
        component_owners[owner].update(direct_owners[path])
        for dependency in dependencies:
            dependency_owner = component_by_path[dependency]
            if owner != dependency_owner:
                component_graph[owner].add(dependency_owner)

    cache = {}

    def closure(component):
        if component not in cache:
            result = set(component_owners[component])
            for dependency in component_graph[component]:
                result.update(closure(dependency))
            cache[component] = result
        return cache[component]

    for component in component_graph:
        closure(component)
    return component_by_path, cache
