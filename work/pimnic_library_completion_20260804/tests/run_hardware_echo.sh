#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pim_repo=${PIM_REPO:-/home/pimnic/ziyu/dpu-backend}
bf3_repo=${BF3_REPO:-/home/cxz/gongsunyangmei/nfs/libr}
read -r -a bf3_exec <<<"${BF3_EXEC:-ssh bf3}"
port=${PORT:-6891}
messages=${MESSAGES:-64}
payload=${PAYLOAD:-1024}
log_dir="${root}/logs/hardware_echo"
mkdir -p "${root}/build/example" "${log_dir}"
cp "${root}/build/runtime_pe" \
	"${root}/build/example/bf_pimnic_runtime"

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
	exec "${pim_repo}/build/example/bf_pimnic_runtime_host" \
		--mode echo --num-dpus 64 --payload "${payload}" \
		--messages "${messages}" --batch 16 --poll-us 10 \
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
	mkdir -p work/pimnic_library_20260731/build/tmp
	TMPDIR='${bf3_repo}/work/pimnic_library_20260731/build/tmp' \
		./build_dpu/pimnic_runtime_bench \
		-serverIp 192.168.100.1 -port '${port}' -logEvery 0
" >"${bf3_log}" 2>&1

wait "${host_pid}"
host_pid=
grep -q "PIM-centric-control-plane PASS" "${host_log}"
grep -q "PIM-centric BF3 PASS" "${bf3_log}"
echo "pimnic hardware echo PASS logs=${log_dir}"
