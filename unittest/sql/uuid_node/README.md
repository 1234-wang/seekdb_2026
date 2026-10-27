# Linux UUID node startup regression

Issue: https://github.com/oceanbase/seekdb/issues/1480

Build seekdb using the normal release build first. On Linux, install PyMySQL
and ensure a C compiler is available. The test runs real server processes and
SQL against temporary databases; allow approximately 6 GiB RAM and 4 GiB free
disk. It never connects to an existing database.

```sh
python3 unittest/sql/uuid_node/test_uuid_node.py \
  --binary build_release/src/observer/seekdb --work-dir /path/to/scratch
```

The normal case checks the physical MAC path and concurrent SQL UUID uniqueness,
version, variant and process-stable node. Linux-only `LD_PRELOAD` faults exercise
socket/enumeration EPERM, an all-zero MAC, interrupted and partial entropy reads,
and fatal entropy EOF/EIO. The shim affects only the spawned test server.
A separate container with `--network none` exercises a real loopback-only
namespace without changing host interfaces; run the normal case there.

For an unchanged-binary reproduction:

```sh
python3 unittest/sql/uuid_node/test_uuid_node.py \
  --binary /path/to/unchanged/seekdb --case test_enumeration_denied
```

That case must fail before the fix because the server exits before SQL readiness.
The full seven-case suite is expected to pass after the fix. This integration
runner is explicitly invoked; it is not automatically registered in CMake or
Bazel. Windows, macOS, Android, post-initialization fork and UUID timestamp
rollback are outside this test's coverage.
