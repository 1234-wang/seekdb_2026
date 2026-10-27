#!/usr/bin/env python3
# Copyright (c) 2025 OceanBase.
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

"""Restart regression for a disposable SeekDB instance.

The caller owns server lifecycle. Run prepare, restart the SAME disposable data
folder with the patched binary, then verify. Restart once more and run stable
to ensure the persisted marker prevents unnecessary package DDL.

Example (credentials inherited through normal mysql-client environment):
  python3 unittest/observer/mysql/test_scheduler_package_upgrade.py --client /path/to/mysql --port 2889 --state-file /tmp/scheduler-upgrade.json prepare
  # Caller stops and restarts its disposable server with the same data dir.
  python3 unittest/observer/mysql/test_scheduler_package_upgrade.py --client /path/to/mysql --port 2889 --state-file /tmp/scheduler-upgrade.json verify
  # Caller restarts once more before stable.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


ROOT = Path(__file__).resolve().parents[3]
PACKAGE_DIR = ROOT / "src/share/inner_table/sys_package"
PACKAGE_QUERY = """SELECT type, flag, schema_version
FROM oceanbase.__all_package
WHERE database_id = (SELECT database_id FROM oceanbase.__all_database WHERE database_name = 'oceanbase')
  AND LOWER(package_name) = 'dbms_scheduler' AND type IN (1, 2)
ORDER BY type;"""


def run_sql(options, sql):
    command = [options.client, "--host=" + options.host,
               "--port=" + str(options.port), "--user=" + options.user,
               "--database=oceanbase", "--batch", "--raw", "--skip-column-names"]
    result = subprocess.run(command, input=sql, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError("mysql-client failed: " + result.stderr.strip())
    return result.stdout


def package_state(options):
    values = {}
    for row in run_sql(options, PACKAGE_QUERY).splitlines():
        kind, flags, version = map(int, row.split("\t"))
        values[str(kind)] = {"flags": flags, "version": version}
    if set(values) != {"1", "2"}:
        raise AssertionError("expected scheduler spec and body, received " + repr(values))
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "verify", "stable"))
    parser.add_argument("--client", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--user", default="root")
    parser.add_argument("--state-file", type=Path, required=True)
    options = parser.parse_args()
    if options.phase == "prepare":
        spec = (PACKAGE_DIR / "dbms_scheduler_mysql.sql").read_text()
        body = (PACKAGE_DIR / "dbms_scheduler_mysql_body.sql").read_text()
        assert "AUTHID CURRENT_USER" in spec
        old_spec = spec.replace(" AUTHID CURRENT_USER", "")
        run_sql(options, "DELIMITER $$\n" + old_spec + "\n$$\n" + body + "\n$$\n")
        state = package_state(options)
        if any(item["flags"] & 4 for item in state.values()):
            raise AssertionError("prepare must persist the old definer-rights package")
        options.state_file.write_text(json.dumps({"before": state}, indent=2))
        print("legacy definer-rights scheduler persisted; restart the same disposable data directory")
        return
    saved = json.loads(options.state_file.read_text())
    deadline = time.monotonic() + 45
    while True:
        state = package_state(options)
        if all(item["flags"] & 4 and not item["flags"] & 1 for item in state.values()):
            break
        if time.monotonic() >= deadline:
            raise AssertionError("restart did not refresh a valid invoker-rights spec AND body: " + repr(state))
        time.sleep(1)
    if options.phase == "verify":
        if any(state[kind]["version"] <= saved["before"][kind]["version"] for kind in state):
            raise AssertionError("both package parts must be refreshed after the first restart")
        saved["after"] = state
        options.state_file.write_text(json.dumps(saved, indent=2))
        print("restart refreshed valid scheduler spec and body to invoker rights")
    else:
        if state != saved["after"]:
            raise AssertionError("already-upgraded package was unnecessarily rewritten on restart")
        print("second restart preserved package versions; marker is idempotent")


if __name__ == "__main__":
    main()
