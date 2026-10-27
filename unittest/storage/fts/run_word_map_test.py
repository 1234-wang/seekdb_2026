#!/usr/bin/env python3
"""Build and run the FTS word-map tests against the existing Release libraries.

The competition checkout omits the native unittest build definitions. Reuse its
actual compile and link flags without changing production CMake or starting a
server. Rebuild Release after changing production code first.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    build = repo / "build_release"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    logs = (args.output_dir or build / "fts-word-map-tests").resolve()
    logs = logs / f"seekdb-fts-unit-{args.label}-{stamp}"
    logs.mkdir(parents=True)
    source = repo / "unittest/storage/fts/test_fts_word_map.cpp"
    sources = [source, *sorted(source.parent.glob("test_fts_word_map_*.cpp"))]
    unit_objects = [logs / (path.stem + ".o") for path in sources]
    binary = logs / "test_fts_word_map"
    entry = next(
        entry
        for entry in json.loads((build / "compile_commands.json").read_text())
        if entry["file"] == str(repo / "src/observer/main.cpp")
    )
    compile_steps = []
    for index, (path, unit_object) in enumerate(zip(sources, unit_objects)):
        compile_command = shlex.split(entry["command"])
        compile_command[compile_command.index("-o") + 1] = str(unit_object)
        compile_command[compile_command.index("-c") + 1] = str(path)
        # Bundled gtest headers have older warning conventions than production.
        compile_command.remove("-Werror")
        compile_steps.append(
            (
                "compile" if index == 0 else "compile-" + path.stem,
                compile_command,
                Path(entry["directory"]),
            )
        )
    link_command = shlex.split(
        (build / "src/observer/CMakeFiles/seekdb.dir/link.txt").read_text()
    )
    link_command = [
        arg for arg in link_command if not arg.startswith("CMakeFiles/ob_main.dir/")
    ]
    link_command[link_command.index("-o") + 1] = str(binary)
    link_command += [*(str(path) for path in unit_objects), "-lgtest"]
    metadata = {
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "dirty_paths": subprocess.check_output(
            ["git", "status", "--short"], cwd=repo, text=True
        ).splitlines(),
        "test_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "additional_test_sha256": {
            str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sources[1:]
        },
        "commands": [],
        "sql_or_official_benchmark": False,
    }
    status = 1
    try:
        for name, command, cwd in (
            *compile_steps,
            ("link", link_command, build / "src/observer"),
            ("test", [str(binary), f"--gtest_output=xml:{logs / 'gtest.xml'}"], logs),
        ):
            with (logs / f"{name}.log").open("w") as output:
                result = subprocess.run(
                    command,
                    cwd=cwd,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    timeout=300,
                )
            metadata["commands"].append(
                {"name": name, "argv": command, "exit_code": result.returncode}
            )
            print(f"{name}: {result.returncode}; {logs / (name + '.log')}", flush=True)
            print(
                (logs / f"{name}.log").read_text(errors="replace")[-5000:], flush=True
            )
            if result.returncode:
                return result.returncode
        metadata["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        status = 0
        return 0
    finally:
        metadata["exit_code"] = status
        (logs / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
