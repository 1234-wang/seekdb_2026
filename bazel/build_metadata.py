import os
import re
from pathlib import Path
import subprocess
import sys


def workspace_status():
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY", ".")
    for key, arguments in (
        ("GIT_REVISION", ["rev-parse", "HEAD"]),
        ("GIT_BRANCH", ["rev-parse", "--abbrev-ref", "HEAD"]),
    ):
        result = subprocess.run(
            ["git", "-C", workspace] + arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            encoding="utf-8",
        )
        value = result.stdout.strip() if result.returncode == 0 else "unknown"
        sys.stdout.buffer.write(
            ("STABLE_SEEKDB_%s %s\n" % (key, value)).encode("utf-8")
        )


def _escape_cpp_string(value):
    """Escape a value for safe insertion into a C++ string literal."""
    escaped = []
    for char in value:
        code = ord(char)
        if char == "\\":
            escaped.append("\\\\")
        elif char == '"':
            escaped.append('\\"')
        elif char == "\n":
            escaped.append("\\n")
        elif char == "\r":
            escaped.append("\\r")
        elif char == "\t":
            escaped.append("\\t")
        elif code < 0x20 or code == 0x7F:
            escaped.append("\\%03o" % code)
        else:
            escaped.append(char)
    return "".join(escaped)


def version_source(template, status_file, output):
    status = dict(
        line.split(" ", 1)
        for line in Path(status_file).read_text(encoding="utf-8").splitlines()
    )
    values = {
        "BUILD_NUMBER": "1",
        "GIT_REVISION": status["STABLE_SEEKDB_GIT_REVISION"],
        "GIT_BRANCH": status["STABLE_SEEKDB_GIT_BRANCH"],
        "BUILD_FLAGS": "RelWithDebInfo",
        "BUILD_INFO": "",
    }
    source = Path(template).read_text(encoding="utf-8")

    # Replace template placeholders only once. Text inserted from a value
    # must not be interpreted as another placeholder.
    placeholder = re.compile(
        r"@(BUILD_NUMBER|GIT_REVISION|GIT_BRANCH|BUILD_FLAGS|BUILD_INFO)@"
    )
    source = placeholder.sub(
        lambda match: _escape_cpp_string(values[match.group(1)]),
        source,
    )

    with open(output, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(source)


if __name__ == "__main__":
    if sys.argv[1] == "status":
        workspace_status()
    else:
        version_source(*sys.argv[2:])
