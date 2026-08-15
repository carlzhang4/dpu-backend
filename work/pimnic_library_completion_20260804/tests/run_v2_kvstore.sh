#!/usr/bin/env bash
# True V2 equivalence for KVStore: run the instrumented copy of the
# frozen benchmarks/kvstore/kvstore_pim_multidpu.cpp on hardware with the
# original kvstore_get_device DPU binary and the full 32 MiB identity
# table, dump every (key, value) GET response, run the PIMNIC library
# path over the same sequential key stream, and compare per-key results
# byte-for-byte.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
repo=${PIM_REPO:-/home/pimnic/ziyu/dpu-backend}
nfs_bf3_work=${NFS_BF3_WORK:-/home/pimnic/ziyu/nfs/libr/work/pimnic_library_completion_20260804}
# Same NFS directory as seen from the BF3 side: the bench runs on the BF3,
# so -appDumpPath must use the BF3-visible path while this script checks
# the pim1-visible mirror.
bf3_work=${BF3_WORK:-/home/cxz/gongsunyangmei/nfs/libr/work/pimnic_library_completion_20260804}
port=${PORT:-7110}
messages=${MESSAGES:-64}
run_id=${RUN_ID:-v2_kvstore}
log_dir="${root}/logs/${run_id}"
mkdir -p "${log_dir}"

ops=$((messages * 64))
ref_dump="${log_dir}/kvstore_ref.bin"
new_dump="${nfs_bf3_work}/build/kvstore_new_${run_id}.bin"
bf3_dump="${bf3_work}/build/kvstore_new_${run_id}.bin"
rm -f "${ref_dump}" "${new_dump}"

# sudo: libr's malloc_2m_numa needs move_pages privileges (the original
# benchmarks were also run privileged).
sudo env UPMEM_RUNTIME_LIBRARY_PATH="${repo}/build/hw" UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib "${root}/build/kvstore_pim_multidpu_ref" -nodeId 0 -iterations "${ops}" \
	-request_per_dpu 64 -max_hash_entry_num 2097152 -dpu_num 1 \
	-port $((port + 400)) \
	-dpuBinary "${repo}/build/benchmarks/kvstore/kvstore_get_device" \
	-dumpPath "${ref_dump}" >"${log_dir}/kvstore_ref.server.log" 2>&1 &
server_pid=$!
trap 'sudo kill '"${server_pid}"' 2>/dev/null || true' EXIT
sleep 2
sudo env UPMEM_RUNTIME_LIBRARY_PATH="${repo}/build/hw" UPMEM_RUNTIME_PACKAGE_LIBRARY_DIRECTORY=/usr/local/lib "${root}/build/kvstore_pim_multidpu_ref" -nodeId 1 -coreOffset 1 \
	-iterations "${ops}" -request_per_dpu 64 -max_hash_entry_num 2097152 \
	-port $((port + 400)) >"${log_dir}/kvstore_ref.client.log" 2>&1
wait "${server_pid}"
trap - EXIT
sudo chown "$(id -u):$(id -g)" "${ref_dump}"
test -s "${ref_dump}"

RUN_ID="${run_id}" MESSAGES="${messages}" APP_DUMP_PATH="${bf3_dump}" \
	"${root}/tests/run_hardware_app.sh" kvstore "${port}"
test -s "${new_dump}"

python3 "${root}/v2ref/compare_dumps.py" kvstore "${ref_dump}" "${new_dump}"
echo "PIMNIC V2 KVSTORE PASS ref=${ref_dump}"
