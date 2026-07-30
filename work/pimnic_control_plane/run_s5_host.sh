#!/usr/bin/env bash
set -euo pipefail

repo=/home/pimnic/ziyu/dpu-backend
log_path=$1
shift

cd "$repo"
export UPMEM_RUNTIME_LIBRARY_PATH="$repo/build/hw"
export UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib
exec ./build/example/bf_pimnic_runtime_host "$@" >"$log_path" 2>&1
