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

"""Reject successful process exits that bypass the lifecycle test assertions."""

import signal
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: run_completed_test.py TEST_BINARY [ARG ...]", file=sys.stderr)
        return 2
    completed = False
    with subprocess.Popen(
        sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    ) as process:
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="", flush=True)
            completed |= line.strip() == "SEEKDB_LIFECYCLE_TESTS_COMPLETED"
        status = process.wait()
    if status != 0:
        if status < 0:
            try:
                name = signal.Signals(-status).name
            except ValueError:
                name = str(-status)
            print(f"Lifecycle test terminated by {name} (returncode={status})", file=sys.stderr)
        else:
            print(f"Lifecycle test returned failure status {status}", file=sys.stderr)
        return status if status > 0 else 1
    if not completed:
        print("Lifecycle test exited without completing its assertions", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
