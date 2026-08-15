#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
quick=${QUICK:-0}
base_port=${PORT:-7270}
if [[ "${quick}" == 1 ]]; then
	rx_messages=128
	echo_messages=256
	stress_messages=128
	two_rank_messages=128
	active_reactivate_ms=50
else
	rx_messages=100000
	echo_messages=1000000
	stress_messages=10000
	two_rank_messages=100000
	active_reactivate_ms=500
fi

MODE=rx-only MESSAGES=${rx_messages} PAYLOAD=1024 BATCH=128 \
	PORT=$((base_port + 0)) RUN_ID=v1_rx_wrap \
	"${root}/tests/run_hardware_paradigm_echo.sh"
MODE=rx-only MESSAGES=${rx_messages} PAYLOAD=1024 BATCH=128 \
	PE_SLOWDOWN=100000 PORT=$((base_port + 1)) RUN_ID=v1_rx_slow \
	"${root}/tests/run_hardware_paradigm_echo.sh"
MODE=echo MESSAGES=${echo_messages} PAYLOAD=1024 BATCH=128 \
	PORT=$((base_port + 2)) RUN_ID=v1_echo_throughput \
	"${root}/tests/run_hardware_paradigm_echo.sh"
MODE=echo MESSAGES=${stress_messages} PAYLOAD=1024 BATCH=128 \
	DEACTIVATE_GROUP=2 REACTIVATE_AFTER_MS=${active_reactivate_ms} \
	PORT=$((base_port + 3)) RUN_ID=v1_active_table \
	"${root}/tests/run_hardware_paradigm_echo.sh"
MODE=echo MESSAGES=${stress_messages} PAYLOAD=1024 BATCH=128 \
	NIC_SLOWDOWN_US=1000 PORT=$((base_port + 4)) RUN_ID=v1_nic_slow \
	"${root}/tests/run_hardware_paradigm_echo.sh"
MODE=echo NUM_DPUS=128 MESSAGES=${two_rank_messages} PAYLOAD=1024 BATCH=128 \
	PORT=$((base_port + 5)) RUN_ID=v1_two_rank \
	"${root}/tests/run_hardware_paradigm_echo.sh"
echo "PIMNIC V1 six-case matrix PASS quick=${quick}"
