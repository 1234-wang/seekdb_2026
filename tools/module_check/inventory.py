"""Read source inventories and derive module header ownership without executing them."""

import ast
from pathlib import Path
import re
import subprocess


HEADER_SUFFIXES = {".def", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tcc"}
COMPILE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
SOURCE_SUFFIXES = HEADER_SUFFIXES | COMPILE_SUFFIXES
INCLUDE_PATTERN = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^">]+)[">]', re.MULTILINE)


class _StructLiterals(ast.NodeTransformer):
    def visit_Call(self, node):
        if isinstance(node.func, ast.Name) and node.func.id == "struct" and not node.args:
            if any(keyword.arg is None for keyword in node.keywords):
                raise ValueError("struct expansion is not supported in an inventory")
            return ast.Dict(
                keys=[ast.Constant(keyword.arg) for keyword in node.keywords],
                values=[self.visit(keyword.value) for keyword in node.keywords],
            )
        return node


def load_inventory(path, names):
    names = set(names)
    values = {}
    for node in ast.parse(path.read_text(), filename=str(path)).body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id in names:
            values[target.id] = ast.literal_eval(_StructLiterals().visit(node.value))
    missing = sorted(names - values.keys())
    if missing:
        raise ValueError("missing inventory assignments: %s" % missing)
    return values


def source_paths(records):
    paths = []
    for record in records:
        if isinstance(record, str):
            paths.append(record)
        elif "srcs" in record:
            paths.extend(record["srcs"])
        else:
            paths.append(record["path"])
    return paths


def tracked_files(repo, root, suffixes):
    output = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", root.relative_to(repo).as_posix()],
        cwd=repo, universal_newlines=True,
    )
    return {
        (repo / name).relative_to(root).as_posix(): repo / name
        for name in output.splitlines()
        if (repo / name).is_file() and Path(name).suffix in suffixes
    }


def files_under(root, suffixes, excluded=()):
    return {
        path.relative_to(root).as_posix(): path
        for path in root.rglob("*")
        if path.is_file() and path.suffix in suffixes
        and path.relative_to(root).as_posix() not in excluded
    }


def _relative_to(path, root):
    try:
        return path.relative_to(root)
    except ValueError:
        return None


def _resolve_header(include, source, module_root, headers):
    candidates = []
    for prefix in (module_root.name + "/", "src/" + module_root.name + "/"):
        if include.startswith(prefix):
            candidates.append(include[len(prefix):])
    source_relative = _relative_to(source, module_root)
    if source_relative is not None:
        candidates.append((source_relative.parent / include).as_posix())
    candidates.append(include)
    for candidate in candidates:
        normalized = str(Path(candidate))
        if normalized in headers:
            return normalized
    return None


def header_ownership(repo, module_root, headers, sources, declared_public=()):
    public = set(declared_public) & set(headers)
    consumers = {}
    edges = {header: set() for header in headers}
    for source in sources:
        source_relative = _relative_to(source, module_root)
        for include in INCLUDE_PATTERN.findall(source.read_text(errors="ignore")):
            header = _resolve_header(include, source, module_root, headers)
            if header is None:
                continue
            if source_relative is None:
                public.add(header)
                consumers.setdefault(header, set()).add(source.relative_to(repo).as_posix())
            elif source_relative.as_posix() in headers:
                edges[source_relative.as_posix()].add(header)
    reachable = set(public)
    pending = list(public)
    while pending:
        header = pending.pop()
        for dependency in edges[header]:
            if dependency not in reachable:
                reachable.add(dependency)
                pending.append(dependency)
    return public, reachable - public, set(headers) - reachable, consumers
