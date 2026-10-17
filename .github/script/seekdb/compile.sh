#!/usr/bin/env bash
set -euo pipefail
workspace="${GITHUB_WORKSPACE:?}"
task_dir="${SEEKDB_TASK_DIR:?}"
cd "$workspace"
mkdir -p "$task_dir"
args=(--jobs="${BUILD_JOBS:-32}")
case "${PACKAGE_TYPE:-release}" in
  release) ;;
  sanity) args+=(--sanity) ;;
  *) echo "Unsupported package type: $PACKAGE_TYPE" >&2; exit 2 ;;
esac
if [[ -n "${BAZEL_DISK_CACHE:-}" ]]; then
  args+=(--disk_cache="$BAZEL_DISK_CACHE" --experimental_disk_cache_gc_max_size=3G)
fi
if [[ -n "${FORWARDING_HOST:-}" ]]; then
  echo "$FORWARDING_HOST mirrors.oceanbase.com" >> /etc/hosts
fi
started=$SECONDS
python3 ./build.py init 2>&1 | tee "$task_dir/compile_init.output"
if [[ -n "${SEEKDB_CACHE_NFS_ROOT:-}" ]]; then
  bash .github/script/seekdb/cache.sh baseline-bazel || echo "Failed to record Bazel cache baseline" >&2
fi
python3 ./build.py build "${args[@]}" //src/observer:seekdb 2>&1 | tee "$task_dir/compile.output"
echo "[compile.sh] total elapsed=$((SECONDS - started))s"
