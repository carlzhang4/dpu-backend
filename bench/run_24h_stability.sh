#!/usr/bin/env bash
set -euo pipefail

bench_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
duration_s=${DURATION_S:-86400}
deadline=$(( $(date +%s) + duration_s ))
iteration=0

while (( $(date +%s) < deadline )); do
	iteration=$((iteration + 1))
	run_id=$(printf "stability_%06d" "${iteration}")
	MODE=echo NUM_DPUS="${NUM_DPUS:-128}" \
		PAYLOAD="${PAYLOAD:-1024}" \
		MESSAGES="${MESSAGES_PER_RUN:-100000}" BATCH=128 \
		PORT=$((6800 + iteration % 100)) RUN_ID="${run_id}" \
		"${bench_dir}/run_control_plane_pair.sh"
done
echo "24h stability PASS iterations=${iteration} duration_s=${duration_s}"
