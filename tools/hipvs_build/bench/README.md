# Reproduce the opt-in hipVS build experiment

This developer tool runs against an **already-owned disposable Linux seekdb process**. It never installs packages, starts/stops a server, selects an existing user schema or deletes a data directory. The default invocation prints a PLAN and does not connect. Execution requires `--execute --disposable`, an explicit absolute directory owned by the caller, an exact PID/start identity, binary SHA/source revision and an endpoint owned by that process. User ports 2881/2883 are rejected. The SQL script creates a unique schema and intentionally retains its last index for normal-reopen checking.

The measured implementation is integrated commit `5ee7271c6f750894a3e1ff15c9d6b2afa5f44ff9`, not an independently built version of this minimal branch. Its two AMD product CPP files and helper code are byte-identical here. See [REPORT.md](REPORT.md) for the real three-round CPU/GPU times, complete Recall, ID/DML checks and normal reopen. The standalone branch remains a draft until its own build and runtime evidence are added. Official dataset/quality/speedup thresholds were not present in the published task and no score is inferred.

## Build prerequisites

Use Linux, the repository's initialized matching compiler/SDK/Rust dependencies, and enough memory for the selected parallelism. No GPU library is linked into the seekdb executable. The optional helper needs the existing competition template's ROCm/hipcc 7.2.4 and hipVS headers/library under `/opt/rocm` and `/opt/hipvs`. Its measured `libcuvs_c.so` digest and image identity are in REPORT.md. The runner needs Python 3.10+ and an already available PyMySQL client. No installation is performed by these tools.

From this branch's repository root, the native build commands are:

```sh
# Initialize repository dependencies first if not already present, following the repository README.
# Put caller-managed Cargo/Rustup/temp caches inside the chosen owned workspace.
bash build.sh release --make -j8
WORK=/absolute/path/to/an/existing-owned-workspace
BIN="$PWD/build_release/src/observer/seekdb"
HELPER="$WORK/seekdb_hipvs_build"
bash tools/hipvs_build/build.sh "$HELPER"
sha256sum "$BIN" "$HELPER"
git rev-parse HEAD
python3 -c 'import pymysql; print(pymysql.__version__)'
```

`WORK` and output targets must belong to this reproduction. Do not replace another developer's binary or directory. These are reproduction instructions; this minimal branch's full native build has not been run yet.

## Exact deterministic input

```sh
python3 tools/hipvs_build/bench/generate_inputs.py
python3 tools/hipvs_build/bench/generate_inputs.py --execute --output "$WORK/inputs"
sha256sum "$WORK/inputs/base.f32" "$WORK/inputs/queries.f32"   "$WORK/inputs/ids.i64" "$WORK/inputs/exact-ground-truth.json"
python3 tools/hipvs_build/bench/validate_sql.py
```

The generator uses only the standard library, creates a new output directory, and produces N=20,000, dim=128, 24 queries, K=10, seed=20261027, noncontiguous SQL IDs and independent exact squared-L2 neighbors. Its unchanged generator was actually run in Python 3.10.12; all four original byte hashes matched. Do not use these synthetic measurements as results on an unpublished official workload.

## New owned process and endpoint

Use a separate new base for CPU and GPU, never a user instance or a concurrently running process. Both modes use the **same executable**, inputs, CPU4/thread settings, default memory-saving behavior and ANN parameters. Start from a shell with no unrelated GPU/VSAG preload configuration. The only intended difference is enabling the explicit helper for GPU. For example, on the official Linux GPU image:

```sh
# MODE=cpu first, MODE=gpu in a separate run after CPU has normally stopped.
MODE=cpu
RUN=$(mktemp -d "$WORK/amd-$MODE.XXXXXXXX")
BASE="$RUN/base"
mkdir -m 700 "$BASE" "$RUN/tmp"
# CPU: unset all three SEEKDB_HIPVS_BUILD_* variables.
unset SEEKDB_HIPVS_BUILD_HELPER SEEKDB_HIPVS_BUILD_TMPDIR SEEKDB_HIPVS_BUILD_TIMEOUT_SECONDS
if [ "$MODE" = gpu ]; then
  export SEEKDB_HIPVS_BUILD_HELPER="$HELPER"
  export SEEKDB_HIPVS_BUILD_TMPDIR="$RUN/tmp"
  export SEEKDB_HIPVS_BUILD_TIMEOUT_SECONDS=300
elif [ "$MODE" != cpu ]; then
  echo "Use cpu or gpu" >&2
  exit 2
fi
OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 TMPDIR="$RUN/tmp"   "$BIN" --nodaemon --base-dir "$BASE" --port 2891 --log-level INFO   --parameter mysql_port_mode=random --parameter memory_budget=2G   --parameter cpu_count=4 --parameter net_thread_count=2   --parameter datafile_size=128M --parameter datafile_maxsize=2G   --parameter log_disk_size=2G --parameter max_syslog_file_count=4   --parameter enable_rpc_service=false --parameter enable_async_syslog=false   > "$RUN/server-stdio.log" 2>&1 &
PID=$!
```

Wait for readiness. Determine `SQL_PORT` from the listening loopback socket owned by **this PID**, not from another service or the configured `@@port`: random-port mode binds an actual loopback endpoint. The runner verifies PID, executable, source revision, start ticks, UID, base, socket ownership and the helper environment before any SQL. It refuses to substitute a different server when identity fails.

The following read-only command derives the port from that process; if it prints no port, wait for readiness, check its own log and repeat this read. It rejects ambiguous or non-loopback listeners:

```sh
SQL_PORT=$(python3 - "$PID" <<'PY'
import os,re,sys
from pathlib import Path
p=Path('/proc')/str(int(sys.argv[1])); sockets=set()
for fd in (p/'fd').iterdir():
    try:
        m=re.fullmatch(r'socket:\[(\d+)\]',os.readlink(fd))
        if m:sockets.add(m[1])
    except FileNotFoundError:pass
ports=[]
for name in ('tcp','tcp6'):
    for line in (p/'net'/name).read_text().splitlines()[1:]:
        f=line.split()
        if f[3]=='0A' and f[9] in sockets:
            address,port=f[1].split(':')
            if name!='tcp' or address!='0100007F':raise SystemExit('unexpected non-loopback listener')
            ports.append(int(port,16))
if len(ports)>1:raise SystemExit('ambiguous owned endpoint')
if ports:print(ports[0])
PY
)
```

## Complete SQL validation and measurement

Run this command for each mode, replacing MODE with that process's actual mode. GPU mode needs the two additional helper arguments shown below; CPU mode omits them.

```sh
EXTRA=()
if [ "$MODE" = gpu ]; then
  EXTRA=(--expected-helper "$HELPER" --expected-helper-sha256 "$(sha256sum "$HELPER" | cut -d' ' -f1)")
fi
python3 tools/hipvs_build/bench/validate_sql.py \
  --execute --disposable --workspace-root "$WORK" \
  --mode "$MODE" --host 127.0.0.1 --port "$SQL_PORT" --pid "$PID" \
  --expected-uid "$(id -u)" --binary "$BIN" \
  --expected-sha256 "$(sha256sum "$BIN" | cut -d' ' -f1)" \
  --expected-revision "$(git rev-parse HEAD)" \
  --base-dir "$BASE" --server-log "$BASE/log/seekdb.log" \
  --inputs "$WORK/inputs" --output "$RUN/sql" --rounds 3 --deadline-seconds 900 "${EXTRA[@]}"
```

The tool loads all original IDs before timing, sets and reads session `ob_hnsw_ef_search=512`, times three **fresh** SQL index builds through first usable ANN query, validates every query's 10 original IDs and Recall against the exact oracle, checks actual incremental GPU/fallback log spans, then tests filtering, rollback, committed vector update, deletion, insertion and primary-key change. Its local supplemental quality floor is 0.95, explicitly not an official threshold. Do not count a CPU fallback as a GPU round.

For normal-reopen verification, record the original report, normally close only this owned instance, and restart the **same base and binary** with the same mode in a new process. Derive its new actual PID/port and run the same validation command with `--resume "$RUN/sql/report.json"` and a **new** output such as `--output "$RUN/sql-reopen"`. No index is rebuilt by resume; it checks all original IDs and all 24 queries on the committed DML state. The runner deliberately does not stop or kill servers; the launcher/operator owns a checked normal shutdown. Preserve the schema/base until this step is complete.

## Evidence and wrapper scope

`evidence/measured-results.json` is a derived summary retaining the six original timings, exact Recall values and actual per-round log hashes/flags; it is not relabeled as an original unmodified report. The six `*-round-*-results.json` files are exact original synthetic query-ID records. Selected ASCII native-import lines are supplied separately; their retained source excerpt hashes do not mean complete mixed-byte excerpts are included. Four original sessions and four original SQL reports remain separately sealed.

The public `validate_sql.py` changes only the old personal workspace restriction to explicit `--workspace-root` and the digest helper to a Python3.10-compatible chunk loop. After removing those wrapper changes, the whole `main` SQL-flow AST matches the actually executed original 9a315 script; all other six helper function ASTs are identical. The public wrapper passed real Python3.10 parsing, default PLAN, missing-root refusal, path-boundary and digest checks; its newly packaged CLI has **not** rerun real SQL. This is why the PR remains draft.
