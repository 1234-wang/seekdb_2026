#!/usr/bin/env bash
set -euo pipefail
export GITHUB_WORKSPACE="${WORKSPACE:?}"
exec bash "$WORKSPACE/.github/script/seekdb/compile.sh"
