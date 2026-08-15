#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
app=${1:?usage: run_hardware_app.sh APP [PORT]}
port=${2:-7100}
messages=${MESSAGES:-4}
pim_repo=${PIM_REPO:-/home/pimnic/ziyu/dpu-backend}
bf3_repo=${BF3_REPO:-/home/cxz/gongsunyangmei/nfs/libr}
bf3_work=${BF3_WORK:-${bf3_repo}/work/pimnic_library_completion_20260804}
read -r -a bf3_exec <<<"${BF3_EXEC:-env BF3_PASSWORD=cxz123 BF3_SSH_TIMEOUT=3600 python3 /tmp/bf3_exec_20260731.py}"
log_dir="${root}/logs/${RUN_ID:-app_${app}}"
mkdir -p "${log_dir}"
host_log="${log_dir}/${app}.host.log"
bf3_log="${log_dir}/${app}.bf3.log"

UPMEM_RUNTIME_LIBRARY_PATH="${pim_repo}/build/hw" \
UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib \
"${root}/build/app_host" --app "${app}" --messages "${messages}" \
	--port "${port}" >"${host_log}" 2>&1 &
host_pid=$!
trap 'kill '"${host_pid}"' 2>/dev/null || true' EXIT
for _ in $(seq 1 300); do
	grep -q "Waiting for client" "${host_log}" 2>/dev/null && break
	if ! kill -0 "${host_pid}" 2>/dev/null; then
		cat "${host_log}"
		exit 1
	fi
	sleep 0.1
done
app_dump=${APP_DUMP_PATH:-}
"${bf3_exec[@]}" "cd '${bf3_repo}' && TMPDIR='${bf3_work}/build/tmp' \
	'${bf3_work}/build/paradigm_runtime_bench' \
	-serverIp 192.168.100.1 -port '${port}' -logEvery 0 \
	-appDumpPath '${app_dump}'" \
	>"${bf3_log}" 2>&1
wait "${host_pid}"
trap - EXIT
grep -Eq "PIMNIC APP PASS app=${app} .*v2_equal=1" "${host_log}"
grep -q "PIMNIC BF3 PASS" "${bf3_log}"
cat "${host_log}" | grep "PIMNIC APP PASS"
echo "pimnic hardware app ${app} PASS logs=${log_dir}"
