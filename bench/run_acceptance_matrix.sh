#!/usr/bin/env bash
set -euo pipefail

bench_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# S5: RX ring, generation wrap, and PE-side backpressure.
MODE=rx-only MESSAGES=100000 PAYLOAD=1024 BATCH=128 \
	PORT=6701 RUN_ID=s5_rx_100k \
	"${bench_dir}/run_control_plane_pair.sh"
MODE=rx-only MESSAGES=100000 PAYLOAD=1024 BATCH=128 \
	PE_SLOWDOWN=100000 PORT=6702 RUN_ID=s5_rx_slow \
	"${bench_dir}/run_control_plane_pair.sh"

# S6: echo, dynamic active table, and TX backpressure.
MODE=echo MESSAGES=1000000 PAYLOAD=1024 BATCH=128 \
	PORT=6703 RUN_ID=s6_echo_1m \
	"${bench_dir}/run_control_plane_pair.sh"
MODE=echo MESSAGES=10000 PAYLOAD=1024 BATCH=128 \
	DEACTIVATE_GROUP=2 REACTIVATE_AFTER_MS=500 PORT=6704 \
	RUN_ID=s6_active_table \
	"${bench_dir}/run_control_plane_pair.sh"
MODE=echo MESSAGES=10000 PAYLOAD=1024 BATCH=128 \
	NIC_SLOWDOWN_US=1000 PORT=6705 RUN_ID=s6_nic_slow \
	"${bench_dir}/run_control_plane_pair.sh"

# S7: two ranks and eight active groups.
MODE=echo NUM_DPUS=128 MESSAGES=100000 PAYLOAD=1024 BATCH=128 \
	PORT=6706 RUN_ID=s7_two_rank \
	"${bench_dir}/run_control_plane_pair.sh"
