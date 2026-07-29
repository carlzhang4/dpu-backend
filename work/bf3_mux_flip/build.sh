#!/usr/bin/env bash
set -euo pipefail

work_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "${work_dir}/../.." && pwd)

cmake -S "${repo_dir}" -B "${repo_dir}/build"
cmake --build "${repo_dir}/build" \
	--target bf3_mux_host generate_output_files -j8
