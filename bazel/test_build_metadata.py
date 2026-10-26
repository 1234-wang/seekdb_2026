import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("build_metadata.py")


class BuildMetadataTest(unittest.TestCase):
    def test_branch_names_are_preserved_and_compile(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("g++ or clang++ is required")

        branch_names = [
            'fix/"quoted"',
            "fix/@BUILD_FLAGS@",
        ]

        for branch in branch_names:
            with self.subTest(branch=branch), tempfile.TemporaryDirectory() as tmp:
                tmp_path = Path(tmp)
                template = tmp_path / "template.cpp.in"
                status = tmp_path / "stable-status.txt"
                output = tmp_path / "generated.cpp"

                template.write_text(
                    """
#include <cstring>
extern "C" {
const char* build_version() {
    return "@BUILD_NUMBER@-@GIT_REVISION@";
}
const char* build_flags() { return "@BUILD_FLAGS@"; }
const char* build_branch() { return "@GIT_BRANCH@"; }
const char* build_info() { return "@BUILD_INFO@"; }
}
int main() {
    return std::strcmp(build_branch(), EXPECTED_BRANCH) == 0 ? 0 : 1;
}
""".replace(
                        "EXPECTED_BRANCH",
                        '"' + branch.replace("\\", "\\\\").replace('"', '\\"') + '"',
                    ),
                    encoding="utf-8",
                )

                status.write_text(
                    "STABLE_SEEKDB_GIT_REVISION abcdef123456\n"
                    f"STABLE_SEEKDB_GIT_BRANCH {branch}\n",
                    encoding="utf-8",
                )

                subprocess.run(
                    [
                        sys.executable,
                        str(SCRIPT),
                        "version",
                        str(template),
                        str(status),
                        str(output),
                    ],
                    check=True,
                )

                executable = tmp_path / "test_metadata"
                subprocess.run(
                    [compiler, "-std=c++11", str(output), "-o", str(executable)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
