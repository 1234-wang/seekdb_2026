# Copyright (c) 2026 张轩博. Licensed under the Apache License, Version 2.0.
"""Start a dedicated seekdb with MAC discovery restricted and test UUIDs."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
import uuid
import pymysql

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, required=True)
args = parser.parse_args()
binary = str(args.binary.resolve())
data = Path(tempfile.mkdtemp(prefix='seekdb-uuid-restricted-', dir='/var/tmp'))
env = dict(os.environ)
if os.environ.get('TEST_UUID_MODE', 'restricted') != 'normal':
    shim = data / 'uuid_restricted_ioctl.so'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    str(Path(__file__).with_name('uuid_restricted_ioctl.c')), '-o', str(shim), '-ldl'], check=True)
    env['LD_PRELOAD'] = str(shim)
print(f'instance: {data}', flush=True)
with (data / 'stdout.log').open('w') as log:
    process = subprocess.Popen([binary, '--nodaemon', '--base-dir', str(data),
        '--parameter', 'mysql_port_mode=random', '--parameter', 'memory_budget=1G',
        '--parameter', 'datafile_size=128M', '--parameter', 'datafile_maxsize=512M',
        '--parameter', 'log_disk_size=2G', '--parameter', 'cpu_count=2'],
        stdout=log, stderr=subprocess.STDOUT, env=env)
    conn = None
    try:
        for _ in range(90):
            if process.poll() is not None:
                raise RuntimeError(f'server exited {process.returncode} before readiness: {data}')
            try:
                conn = pymysql.connect(unix_socket=str(data / 'run/sql.sock'), user='root')
                break
            except pymysql.Error:
                time.sleep(1)
        if conn is None:
            raise RuntimeError('server readiness timed out')
        with conn.cursor() as cur:
            cur.execute('SELECT UUID(), 1')
            first, one = cur.fetchone()
            cur.execute('SELECT UUID()')
            second = cur.fetchone()[0]
            first, second = uuid.UUID(first), uuid.UUID(second)
            print('UUID versions:', first.version, second.version, 'distinct:', first != second, flush=True)
            assert one == 1 and first.version == second.version == 1 and first != second
            if os.environ.get('TEST_UUID_MODE', 'restricted') != 'normal':
                assert first.node & (1 << 40), 'random node must set multicast bit'
        print('PASS: startup and two distinct valid v1 UUIDs', flush=True)
    finally:
        if conn:
            conn.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
