#!/usr/bin/env bash
set -euo pipefail

work_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "${work_dir}/../.." && pwd)

mode=${1:?usage: run_host.sh x1|x2|x3|x4|x5|x6 [iterations] [port]}
iterations=${2:-}
port=${3:-6677}

perfctrlsts=$(sudo -n setpci -s 0000:14:00.0 180.l)
if (( (16#${perfctrlsts} & 16#80) != 0 )); then
	echo "DDIO allocating-flow is enabled on 0000:14:00.0 (${perfctrlsts})."
	echo "Run: ${work_dir}/ddio_root_port.sh off"
	exit 1
fi

arguments=(--mode "${mode}" --port "${port}")
if [[ -n "${iterations}" ]]; then
	arguments+=(--iterations "${iterations}")
fi

cd "${repo_dir}"
UPMEM_RUNTIME_LIBRARY_PATH="${repo_dir}/build/hw" \
UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib \
	exec "${repo_dir}/build/example/bf3_mux_host" "${arguments[@]}"
