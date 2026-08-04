#!/usr/bin/env bash
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
repo=${PIM_REPO:-/home/pimnic/ziyu/dpu-backend}
log="${root}/logs/v1_matrix.log"
status="${root}/logs/v1_matrix.status"
mkdir -p "${root}/logs"

cd "${repo}"
BF3_SSH="env BF3_PASSWORD=${BF3_PASSWORD:?missing BF3_PASSWORD} BF3_SSH_TIMEOUT=3600 python3 /tmp/bf3_exec_20260731.py" \
	./bench/run_acceptance_matrix.sh >"${log}" 2>&1
rc=$?
if [[ ${rc} -eq 0 ]]; then
	printf 'PASS\n' >"${status}"
else
	printf 'FAIL rc=%d\n' "${rc}" >"${status}"
fi
exit "${rc}"
