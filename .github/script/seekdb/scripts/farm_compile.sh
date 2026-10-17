#!/usr/bin/env bash
# Farm compile: keep the CMake output layout consumed by the farm test runners.
# Expects: WORKSPACE, SEEKDB_TASK_DIR, PACKAGE_TYPE (release|sanity), REPO=server
set -e

cd "$WORKSPACE"
build_type="${PACKAGE_TYPE:-release}"
sanity=OFF
case "$build_type" in
  release) ;;
  sanity) sanity=ON ;;
  *) echo "Unsupported CMake build type: $build_type" >&2; exit 2 ;;
esac
python3 ./build.py init
export PATH="$WORKSPACE/deps/3rd/usr/local/oceanbase/devtools/bin:$PATH"
build_dir="build_${build_type}"
cmake -S . -B "$build_dir" -G "Unix Makefiles" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DENABLE_SANITY="$sanity"
cmake --build "$build_dir" --parallel "$(nproc)" --target seekdb
