#!/usr/bin/env python3
"""Exercise SQL and signal shutdown through the executable's shared SDK path."""
import pathlib
import signal
import subprocess
import sys
import tempfile
import time

binary, mysql = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='seekdb-engine-process-') as temporary:
    directory = pathlib.Path(temporary)
    with (directory / 'host.log').open('w+') as log:
        process = subprocess.Popen([
            binary, '--nodaemon', '--base-dir', temporary,
            '--parameter', 'memory_budget=1G', '--parameter', 'log_disk_size=2G',
            '--parameter', 'mysql_port_mode=disabled', '--parameter', 'cpu_count=2',
        ], stdout=log, stderr=subprocess.STDOUT)
        try:
            client = [mysql, '--protocol=socket', '--socket=' + str(directory / 'run/sql.sock'),
                      '--user=root', '--batch', '--skip-column-names', '--connect-timeout=2']
            deadline = time.monotonic() + 100
            while True:
                if process.poll() is not None:
                    raise RuntimeError('SeekDB exited during startup: ' + str(process.returncode))
                result = subprocess.run(client + ['-e', 'SELECT 1'], capture_output=True, text=True, timeout=10)
                if result.returncode == 0:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError('Readiness timed out: ' + result.stderr)
                time.sleep(.2)
            query = ('CREATE DATABASE sdk_smoke; '
                     'CREATE TABLE sdk_smoke.t (id INT PRIMARY KEY, value VARCHAR(20)); '
                     "INSERT INTO sdk_smoke.t VALUES (1, 'sdk'); "
                     'SELECT id, value FROM sdk_smoke.t')
            result = subprocess.run(client + ['-e', query], capture_output=True, text=True, timeout=30, check=True)
            assert result.stdout.strip() == '1\tsdk', result.stdout
            process.send_signal(signal.SIGTERM)
            # Previously this force-killed the process, or blocked in package
            # loading until the schema-refresh wait's 100-second timeout.
            assert process.wait(timeout=30) == 0
            assert not (directory / 'run/seekdb.pid').exists()
            print('SQL and SIGTERM shutdown completed')
        except BaseException:
            log.flush()
            log.seek(0)
            print(log.read())
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
