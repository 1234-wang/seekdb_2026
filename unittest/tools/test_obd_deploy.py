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

"""Run the real deployment CLI against an isolated, scripted OBD executable.

Usage: python3 -m unittest discover -s unittest/tools -p 'test_obd_deploy.py' -v
No server, OBD installation, network, or access to existing deployments is needed.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


FAKE_OBD = r'''
import json
import os
from pathlib import Path
import sys

state_path = Path(os.environ["TEST_OBD_STATE"])
state = json.loads(state_path.read_text())
args = sys.argv[1:]
state["calls"].append(args)
ret = 0
if args[:2] == ["mirror", "create"]:
    print("md5: test-package")
elif args[:2] == ["cluster", "start"]:
    # Exhausting the plan must fail, rather than accidentally hiding a retry.
    ret, status = state["starts"].pop(0) if state["starts"] else (99, "UNCHNAGE")
    cluster = Path(os.environ["_OBD_HOME"]) / ".obd" / "cluster" / "regression"
    cluster.mkdir(parents=True, exist_ok=True)
    (cluster / ".data").write_text("config_status: " + status + "\n")
    if ret:
        print("injected OBD startup failure", file=sys.stderr)
elif args[:2] == ["test", "mysqltest"]:
    ret = state["init_ret"]
state_path.write_text(json.dumps(state))
sys.exit(ret)
'''


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "requires POSIX Bash")
class DeployExitStatusTest(unittest.TestCase):
    def run_deploy(self, starts, *, init=True, noninteractive=False, replies="", init_ret=0):
        source = Path(__file__).resolve().parents[2] / "tools" / "deploy"
        with tempfile.TemporaryDirectory(prefix="seekdb-obd-test-") as tmp:
            root = Path(tmp)
            deploy = root / "tools" / "deploy"
            for folder in ("bin", "etc", "admin", ".obd/cluster"):
                (deploy / folder).mkdir(parents=True)
            # Preserve the complete CLI and activation logic, normalizing only
            # checkout line endings for Bash (e.g. a Windows-mounted checkout).
            for name in ("obd.sh", "activate_obd.sh"):
                (deploy / name).write_text((source / name).read_text())
            binary = deploy / "bin" / "seekdb"
            binary.write_text("#!/bin/bash\necho 'seekdb 1.4.0.0'\n")
            binary.chmod(0o700)
            fake_obd = root / "fake-obd"
            fake_obd.write_text("#!" + sys.executable + "\n" + FAKE_OBD)
            fake_obd.chmod(0o700)
            config = deploy / "single.yaml"
            config.write_text("seekdb: {}\n")
            # Prevent activation from reading user configuration or profiles.
            environ = os.environ.copy()
            for name in ("BASH_ENV", "ENV", "_OBD_PROFILE", "OB_FLOW_WORK_DIR"):
                environ.pop(name, None)
            environ.update(
                _OBD_BIN=str(fake_obd),
                _OBD_HOME=str(deploy),
                _OBD_INSTALL_PRE=str(root / "deps"),
                OB_DO_GLOBAL_CONFIG=str(root / "no-global-config"),
                OB_DO_NO_GLOBAL_CLUSTER="1",
                TEST_OBD_STATE=str(root / "state.json"),
            )
            state_path = root / "state.json"
            state_path.write_text(json.dumps(dict(starts=starts, calls=[], init_ret=init_ret)))
            (deploy / ".obd" / ".obd_environ").write_text(
                json.dumps(dict(OBD_DEV_MODE="1", OBD_DEPLOY_BASE_DIR=str(deploy)))
            )
            command = [
                "bash", str(deploy / "obd.sh"), "deploy", "-n", "regression",
                "-c", str(config), "--exec-init-sql", "1" if init else "0",
            ]
            if noninteractive:
                command.append("-N")
            completed = subprocess.run(
                command, input=replies, text=True, capture_output=True,
                env=environ, cwd=root, timeout=10,
            )
            state = json.loads(state_path.read_text())
        calls = state["calls"]
        self.assertIn(
            ["cluster", "deploy", "regression", "--force", "--clean", "--config", str(config)], calls,
        )
        start_calls = [call for call in calls if call[:2] == ["cluster", "start"]]
        init_calls = [call for call in calls if call[:2] == ["test", "mysqltest"]]
        self.assertEqual(len(start_calls), len(starts) - len(state["starts"]), completed)
        return completed, calls, init_calls

    def test_noninteractive_start_failure_never_initializes_or_returns_success(self):
        for init in (False, True):
            with self.subTest(init=init):
                result, calls, init_calls = self.run_deploy(
                    [(7, "NEED_REDEPLOY")], init=init, noninteractive=True,
                )
                self.assertEqual(result.returncode, 7, result)
                self.assertEqual(init_calls, [])
                self.assertFalse(any(call[:2] == ["cluster", "edit-config"] for call in calls))

    def test_failure_without_redeploy_status_is_propagated(self):
        result, _, init_calls = self.run_deploy([(8, "UNCHNAGE")], init=False)
        self.assertEqual(result.returncode, 8, result)
        self.assertEqual(init_calls, [])

    def test_initial_start_success_preserves_init_option(self):
        for init in (False, True):
            with self.subTest(init=init):
                result, _, init_calls = self.run_deploy([(0, "UNCHNAGE")], init=init)
                self.assertEqual(result.returncode, 0, result)
                self.assertEqual(len(init_calls), int(init))

    def test_successful_interactive_retry_still_initializes(self):
        for replies in ("y\n", "\n"):
            with self.subTest(replies=replies):
                result, calls, init_calls = self.run_deploy(
                    [(7, "NEED_REDEPLOY"), (0, "UNCHNAGE")], replies=replies,
                )
                self.assertEqual(result.returncode, 0, result)
                self.assertIn(["cluster", "edit-config", "regression"], calls)
                self.assertEqual(len(init_calls), 1)

    def test_failed_retry_propagates_latest_failure(self):
        result, _, init_calls = self.run_deploy(
            [(7, "NEED_REDEPLOY"), (9, "UNCHNAGE")], replies="y\n",
        )
        self.assertEqual(result.returncode, 9, result)
        self.assertEqual(init_calls, [])

    def test_multiple_retries_can_recover(self):
        result, calls, init_calls = self.run_deploy(
            [(7, "NEED_REDEPLOY"), (9, "NEED_REDEPLOY"), (0, "UNCHNAGE")],
            replies="y\ny\n",
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertEqual(calls.count(["cluster", "edit-config", "regression"]), 2)
        self.assertEqual(len(init_calls), 1)

    def test_declining_retry_fails_without_initializing(self):
        result, _, init_calls = self.run_deploy([(7, "NEED_REDEPLOY")], replies="n\n")
        self.assertNotEqual(result.returncode, 0, result)
        self.assertEqual(init_calls, [])

    def test_eof_does_not_loop_or_initialize(self):
        result, calls, init_calls = self.run_deploy([(7, "NEED_REDEPLOY")])
        self.assertEqual(result.returncode, 7, result)
        self.assertEqual(init_calls, [])
        self.assertFalse(any(call[:2] == ["cluster", "edit-config"] for call in calls))

    def test_initialization_failure_still_propagates(self):
        result, _, init_calls = self.run_deploy([(0, "UNCHNAGE")], init_ret=6)
        self.assertEqual(result.returncode, 6, result)
        self.assertEqual(len(init_calls), 1)


if __name__ == "__main__":
    unittest.main()
