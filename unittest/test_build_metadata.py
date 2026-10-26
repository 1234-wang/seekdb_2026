#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Regression tests for generated C++ build metadata."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "seekdb_build_metadata", ROOT / "bazel" / "build_metadata.py"
)
build_metadata = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_metadata)


class BuildMetadataTest(unittest.TestCase):
    def generate_source(self, branch):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template = root / "version.cpp.in"
            status = root / "stable-status.txt"
            output = root / "version.cpp"
            template.write_text(
                'const char* build_branch() { return "@GIT_BRANCH@"; }\n'
                'const char* build_flags() { return "@BUILD_FLAGS@"; }\n'
                'const char* build_version() { return "@BUILD_NUMBER@-@GIT_REVISION@"; }\n',
                encoding="utf-8",
            )
            status.write_text(
                "STABLE_SEEKDB_GIT_REVISION abc123\n"
                "STABLE_SEEKDB_GIT_BRANCH " + branch + "\n",
                encoding="utf-8",
            )
            build_metadata.version_source(template, status, output)
            return output.read_text(encoding="utf-8")

    def test_ordinary_branch_preserves_existing_metadata(self):
        self.assertEqual(
            self.generate_source("feature/example"),
            'const char* build_branch() { return "feature/example"; }\n'
            'const char* build_flags() { return "RelWithDebInfo"; }\n'
            'const char* build_version() { return "1-abc123"; }\n',
        )

    def test_quoted_branch_is_a_single_escaped_cpp_string(self):
        branch = 'fix/"quoted"'
        subprocess.run(["git", "check-ref-format", "--branch", branch],
                       check=True, stdout=subprocess.DEVNULL)
        self.assertEqual(
            self.generate_source(branch).splitlines()[0],
            "const char* build_branch() { return "
            + json.dumps(branch) + "; }",
        )

    def test_template_tokens_in_branch_are_not_expanded(self):
        branch = "fix/@BUILD_FLAGS@"
        subprocess.run(["git", "check-ref-format", "--branch", branch],
                       check=True, stdout=subprocess.DEVNULL)
        self.assertEqual(
            self.generate_source(branch).splitlines()[0],
            'const char* build_branch() { return "' + branch + '"; }',
        )


if __name__ == "__main__":
    unittest.main()
