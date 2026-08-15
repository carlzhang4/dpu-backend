#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pim_repo=${PIM_REPO:-/home/pimnic/ziyu/dpu-backend}
bf3_repo=${BF3_REPO:-/home/cxz/gongsunyangmei/nfs/libr}
read -r -a bf3_exec <<<"${BF3_EXEC:-env BF3_PASSWORD=cxz123 BF3_SSH_TIMEOUT=3600 python3 /tmp/bf3_exec_20260731.py}"
bf3_work=${BF3_WORK:-${bf3_repo}/work/pimnic_library_completion_20260804}
port=${PORT:-6892}
messages=${MESSAGES:-64}
payload=${PAYLOAD:-1024}
mode=${MODE:-echo}
num_dpus=${NUM_DPUS:-64}
batch=${BATCH:-16}
pe_slowdown=${PE_SLOWDOWN:-0}
deactivate_group=${DEACTIVATE_GROUP:--1}
reactivate_ms=${REACTIVATE_AFTER_MS:-0}
nic_slowdown_us=${NIC_SLOWDOWN_US:-0}
run_id=${RUN_ID:-hardware_paradigm_echo}
log_dir="${root}/logs/${run_id}"
dpu_binary=paradigm_echo_pe
if [[ "${mode}" == rx-only ]]; then
	dpu_binary=runtime_pe
fi
mkdir -p "${log_dir}"

host_log="${log_dir}/host.log"
bf3_log="${log_dir}/bf3.log"
host_pid=
cleanup()
{
	if [[ -n "${host_pid}" ]] && kill -0 "${host_pid}" 2>/dev/null; then
		kill "${host_pid}" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

(
	cd "${root}"
	export UPMEM_RUNTIME_LIBRARY_PATH="${pim_repo}/build/hw"
	export UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib
	exec "${root}/build/runtime_host" \
		--binary "${root}/build/${dpu_binary}" \
		--mode "${mode}" --num-dpus "${num_dpus}" --payload "${payload}" \
		--messages "${messages}" --batch "${batch}" --poll-us 10 \
		--pe-slowdown "${pe_slowdown}" \
		--deactivate-group "${deactivate_group}" \
		--reactivate-ms "${reactivate_ms}" \
		--port "${port}"
) >"${host_log}" 2>&1 &
host_pid=$!

for _ in $(seq 1 300); do
	if grep -q "Waiting for client" "${host_log}" 2>/dev/null; then
		break
	fi
	if ! kill -0 "${host_pid}" 2>/dev/null; then
		cat "${host_log}"
		exit 1
	fi
	sleep 0.1
done

"${bf3_exec[@]}" "
	set -e
	cd '${bf3_repo}'
	TMPDIR='${bf3_work}/build/tmp' \
		'${bf3_work}/build/paradigm_runtime_bench' \
		-serverIp 192.168.100.1 \
		-port '${port}' -logEvery 0 -nicSlowdownUs '${nic_slowdown_us}'
" >"${bf3_log}" 2>&1

wait "${host_pid}"
host_pid=
grep -q "PIMNIC runtime host PASS" "${host_log}"
grep -q "PIMNIC BF3 PASS" "${bf3_log}"
echo "pimnic paradigm hardware echo PASS logs=${log_dir}"
