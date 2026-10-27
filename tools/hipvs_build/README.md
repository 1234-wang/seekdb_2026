# Isolated hipVS HNSW build helper

The helper builds an L2 float32 CAGRA base graph on the actual AMD GPU, explicitly synchronizes it, constructs only the HNSW upper layers on the CPU, and serializes a legacy HNSW file. It never links VSAG, calls a CPU full graph Build, starts seekdb, accesses the network or installs dependencies.

The seekdb integration is explicitly enabled by `SEEKDB_HIPVS_BUILD_HELPER`, pointing to this locally compiled executable, and an absolute `SEEKDB_HIPVS_BUILD_TMPDIR` (or `TMPDIR`) under an owned workspace. Without the helper variable, the original CPU path remains active. Dense float32 L2 HNSW snapshots without extra-info buffers defer construction until serialization, then import the helper's graph and relabel every row offset to the native VID before publication. Unsupported types and metrics retain their original path. Helper or import failure logs an explicit CPU fallback; successful CPU fallback is not GPU acceleration evidence. The subprocess is isolated from VSAG/OpenMP runtimes, bounded by a timeout, and uses a private scratch directory. No public port or service is created.

## Parent protocol

Use a direct `posix_spawn`/`execve` argument array, never shell concatenation. Named arguments have this exact order:

```text
HELPER --input RAW_ABSOLUTE_F32 --output NEW_ABSOLUTE_HNSW --rows N --dim D --m M --ef-construction EF --host-threads 1
```

Threads may be1 or4. `N>4M`, `1<=D<=65536`, `2<=M<=512`, `M<=EF<=1000000`; raw size must be exactly `N*D*4`, finite native-endian IEEE float32, without overflow and at most8GiB. Very small/unsupported configurations fail explicitly so the parent may use its original CPU path. Host/cgroup and GPU free-memory estimates reject clearly oversized jobs; these checks are conservative estimates, not a guaranteed allocation budget. There is no benchmark-specific N/dimension/M constant. Metric is L2 only; the parent must keep other metrics/types on its supported original path.

The parent creates an exclusive owned scratch directory (prefer0700), writes/closes the raw input, and captures bounded stdout/stderr. The output parent must be owned and not writable by other users. The helper opens input read-only/no-follow, verifies shape and identity before/after reading, validates every float, stages serialization in its own0600 file, and atomically links to the requested output without overwriting. It rejects an already-loaded VSAG/private SDK runtime. On a fatal uncaught vendor exception, the terminate handler prints at most24 stack frames and exits134; it does not diagnose the cause. Parent timeout/cancellation must govern the child process; the helper is never launched in-process.

Success exit0 means `GPU_HNSW_FILE_READY`: GPU synchronized graph+CPU upper layers+serialization completed and the new file was published. It does **not** certify VSAG import, original-ID preservation, recall, seekdb persistence or contest completion. Labels are dense row offsets; the parent must relabel/map them to its Dataset IDs before native operations. Exit2 is invalid input/configuration; exit1 is a caught runtime failure; exit134 is an uncaught vendor failure. All failures are distinct from a successful CPU fallback performed by the parent. The parent must require both exit0 and a valid new file, and account for file export/import plus first usable query in end-to-end time.

## Existing SDK build and short cloud validation

Only the two source files are uploaded; use already-installed `/opt/hipvs` and `/opt/rocm`. Run in a new owned directory; no package manager or full seekdb build is involved:

```bash
JOB=$(mktemp -d /workspace/amd-native-helper.XXXXXXXX)
timeout 180s bash /workspace/seekdb/tools/hipvs_build/build.sh "$JOB/seekdb_hipvs_build"
env -u LD_PRELOAD LD_LIBRARY_PATH=/opt/hipvs/lib:/opt/rocm/lib \
  timeout 120s "$JOB/seekdb_hipvs_build" \
    --input /workspace/amd-component-20261027-r3/gpu-run/base.f32 \
    --output "$JOB/index.hnsw" --rows 20000 --dim 128 --m 32 \
    --ef-construction 200 --host-threads 1 > "$JOB/helper.jsonl" 2> "$JOB/helper.stderr"
```

The concrete shape above is only an example consuming the existing R3 input bytes; derive actual arguments from the seekdb Dataset in integration. Do not put the SDK's copied VSAG/libgomp/gfortran directories or `LD_PRELOAD` into this child environment. Keep helper/build logs and hashes. A terminated serialization can leave only the helper's new `.hipvs-stage-*` file in the owned scratch directory; the parent can clean its own job after collecting evidence, never arbitrary paths.

## Recorded component validation

On 2026-10-27, this exact helper source (`e8d1669fd9dc16b1d8724095ad9d2a7be1b58296395a7676fa630cb02fee02be`) compiled and ran on the official image with ROCm7.2.4 and an AMD gfx1100 GPU. The default8MiB stack succeeded. For20,000 float32 vectors of128 dimensions, M32/efConstruction200, one observed run took1.30468s GPU construction,0.111776s CPU upper layers,0.0307696s serialization/publication, and1.53349s total helper time. The graph contained15,766,424 bytes. These are a single component sample, not whole-seekdb timings.

An independent CPU-only native VSAG probe imported that exact output and passed two scenarios: noncontiguous original IDs and overlapping row-offset/signed64-extreme IDs. Each checked all20,000 original-ID distances,24queries at recall@10=1, native serialize/reopen result identity, min/max IDs and deletion by original ID. The GPU process did not load VSAG. Actual seekdb DDL, storage persistence and end-to-end acceleration still require separate product validation; these component results do not certify the contest task.
