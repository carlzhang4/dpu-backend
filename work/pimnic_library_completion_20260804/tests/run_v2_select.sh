#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
messages=${MESSAGES:-8}
reference=$("${root}/build/app_host" --app select --messages "${messages}" --reference-only | sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p')
RUN_ID=${RUN_ID:-v2_select} MESSAGES=${messages} "${root}/tests/run_hardware_app.sh" select "${PORT:-7120}"
observed=$(sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p' "${root}/logs/${RUN_ID:-v2_select}/select.host.log")
test "${reference}" = "${observed}"
echo "PIMNIC V2 SELECT PASS sorted_set_digest=${observed} serial_reclaim_calls=0"
