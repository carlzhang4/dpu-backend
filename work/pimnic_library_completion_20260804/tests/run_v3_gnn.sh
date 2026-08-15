#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_ID=${RUN_ID:-v3_gnn} MESSAGES=${MESSAGES:-8} "${root}/tests/run_hardware_app.sh" gnn "${PORT:-7140}"
line=$(grep "PIMNIC APP PASS" "${root}/logs/${RUN_ID:-v3_gnn}/gnn.host.log")
pim_repo=${PIM_REPO:-/home/pimnic/ziyu/dpu-backend}
original_log="${root}/logs/${RUN_ID:-v3_gnn}/gnn_original_citeseer.log"
(
	cd "${pim_repo}/build"
	export UPMEM_RUNTIME_LIBRARY_PATH="${pim_repo}/build/hw"
	export UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib
	./benchmarks/GNN/GNN_host_int32-0.0 64 citeseer 16 0
) >"${original_log}" 2>&1
original_ms=$(sed -n 's/total exec\. time = \([0-9.]*\)/\1/p' "${original_log}")
test -n "${original_ms}"
reference_ns=$(sed -n 's/.*reference_ns=\([0-9]*\).*/\1/p' <<<"${line}")
elapsed_ns=$(sed -n 's/.*elapsed_ns=\([0-9]*\).*/\1/p' <<<"${line}")
cpu_pct=$(sed -n 's/.*host_data_cpu_pct=\([0-9.]*\).*/\1/p' <<<"${line}")
ci=$(sed -n 's/.*ci=\([0-9]*\).*/\1/p' <<<"${line}")
printf 'metric,reference_cpu,new_pimnic\n'
printf 'elapsed_ns,%s,%s\n' "${reference_ns}" "${elapsed_ns}"
printf 'host_data_cpu_pct,100,%s\n' "${cpu_pct}"
printf 'steady_ci_commands,0,%s\n' "${ci}"
printf 'original_citeseer_end_to_end_ms,%s,NA\n' "${original_ms}"
echo "PIMNIC V3 GNN PASS original_log=${original_log}"
