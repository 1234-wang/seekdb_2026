#!/usr/bin/env python3
"""A process exit inside the SDK must not turn an incomplete test into a pass."""
import subprocess
import sys

result = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, timeout=240)
print(result.stdout, end='')
if result.returncode != 0:
    sys.exit(result.returncode if result.returncode > 0 else 1)
sys.exit(0 if 'SEEKDB_ENGINE_TEST_COMPLETED' in result.stdout else 1)
