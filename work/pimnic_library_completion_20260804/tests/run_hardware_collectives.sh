#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pim_repo=${PIM_REPO:-/home/pimnic/ziyu/dpu-backend}
bf3_repo=${BF3_REPO:-/home/cxz/gongsunyangmei/nfs/libr}
bf3_work=${BF3_WORK:-${bf3_repo}/work/pimnic_library_completion_20260804}
read -r -a bf3_exec <<<"${BF3_EXEC:-env BF3_PASSWORD=cxz123 BF3_SSH_TIMEOUT=3600 python3 /tmp/bf3_exec_20260731.py}"
base_port=${PORT:-6910}
log_dir="${root}/logs/${RUN_ID:-hardware_collectives}"
mkdir -p "${log_dir}"

run_case()
{
	local name=$1 prim=$2 writeback=$3 dim=$4 dim0=$5 dim1=$6 port=$7
	local host_log="${log_dir}/${name}.host.log"
	local bf3_log="${log_dir}/${name}.bf3.log"
	UPMEM_RUNTIME_LIBRARY_PATH="${pim_repo}/build/hw" \
	UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib \
	"${root}/build/collective_host" \
		--binary "${root}/build/collective_pe" --num-dpus 64 \
		--prim "${prim}" --writeback "${writeback}" --root 0 \
		--bytes 8 --rounds 3 --dim "${dim}" \
		--dim0 "${dim0}" --dim1 "${dim1}" --port "${port}" \
		>"${host_log}" 2>&1 &
	local host_pid=$!
	trap 'kill '"${host_pid}"' 2>/dev/null || true' RETURN
	for _ in $(seq 1 300); do
		grep -q "Waiting for client" "${host_log}" 2>/dev/null && break
		if ! kill -0 "${host_pid}" 2>/dev/null; then
			cat "${host_log}"
			return 1
		fi
		sleep 0.1
	done
	"${bf3_exec[@]}" "cd '${bf3_repo}' && TMPDIR='${bf3_work}/build/tmp' \
		'${bf3_work}/build/paradigm_runtime_bench' \
		-serverIp 192.168.100.1 -port '${port}' -logEvery 0" \
		>"${bf3_log}" 2>&1
	wait "${host_pid}"
	trap - RETURN
	grep -q "PIMNIC collective host PASS" "${host_log}"
	grep -q "PIMNIC BF3 PASS" "${bf3_log}"
	echo "collective ${name} PASS"
}

case_index=0
for prim in 1 2 3 4; do
	for writeback in 0 1; do
		port=$((base_port + case_index))
		run_case "prim${prim}_wb${writeback}" "${prim}" "${writeback}" \
			0 64 0 "${port}"
		case_index=$((case_index + 1))
	done
done
run_case "cross_dim_gather" 3 1 1 16 4 $((base_port + case_index))
echo "pimnic hardware collective matrix PASS logs=${log_dir}"
