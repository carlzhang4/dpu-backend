#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bf3_repo=${BF3_REPO:-/home/cxz/gongsunyangmei/nfs/libr}
read -r -a bf3_ssh <<<"${BF3_SSH:-ssh bf3}"
mode=${MODE:-echo}
num_dpus=${NUM_DPUS:-64}
payload=${PAYLOAD:-1024}
messages=${MESSAGES:-1000000}
batch=${BATCH:-128}
poll_us=${POLL_US:-10}
port=${PORT:-6666}
run_id=${RUN_ID:-$(date +%Y%m%d_%H%M%S)}
log_dir="${repo_dir}/work/pimnic_control_plane/logs/${run_id}"
mkdir -p "${log_dir}"

host_log="${log_dir}/host.log"
bf3_log="${log_dir}/bf3.log"
host_pid=

cleanup()
{
	if [[ -n "${host_pid}" ]] && kill -0 "${host_pid}" 2>/dev/null; then
		kill "${host_pid}" 2>/dev/null || true
		for _ in $(seq 1 10); do
			kill -0 "${host_pid}" 2>/dev/null || return
			sleep 0.1
		done
		kill -KILL "${host_pid}" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

host_args=(
	--mode "${mode}"
	--num-dpus "${num_dpus}"
	--payload "${payload}"
	--messages "${messages}"
	--batch "${batch}"
	--poll-us "${poll_us}"
	--port "${port}"
)
if [[ -n "${PE_SLOWDOWN:-}" ]]; then
	host_args+=(--pe-slowdown "${PE_SLOWDOWN}")
fi
if [[ -n "${NIC_SLOWDOWN_US:-}" ]]; then
	host_args+=(--nic-slowdown-us "${NIC_SLOWDOWN_US}")
fi
if [[ -n "${ACTIVE_MASK:-}" ]]; then
	host_args+=(--active-mask "${ACTIVE_MASK}")
fi
if [[ -n "${DEACTIVATE_GROUP:-}" ]]; then
	host_args+=(--deactivate-group "${DEACTIVATE_GROUP}")
	host_args+=(--reactivate-after-ms "${REACTIVATE_AFTER_MS:-200}")
fi

(
	cd "${repo_dir}"
	export UPMEM_RUNTIME_LIBRARY_PATH="${repo_dir}/build/hw"
	export UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib
	exec "${repo_dir}/build/example/bf_pimnic_runtime_host" \
		"${host_args[@]}"
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

"${bf3_ssh[@]}" "
	set -e
	cd '${bf3_repo}'
	mkdir -p work/pimnic_control_plane/tmp
	TMPDIR='${bf3_repo}/work/pimnic_control_plane/tmp' \
		./build_dpu/pimnic_runtime_bench \
		-serverIp 192.168.100.1 -port '${port}' \
		-logEvery '${LOG_EVERY:-10000}'
" >"${bf3_log}" 2>&1

wait "${host_pid}"
host_pid=
grep -q "PIM-centric-control-plane PASS" "${host_log}"
grep -q "PIM-centric BF3 PASS" "${bf3_log}"
echo "PASS logs=${log_dir}"
