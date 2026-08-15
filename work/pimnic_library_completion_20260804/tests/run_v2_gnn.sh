#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
messages=${MESSAGES:-4}
reference=$("${root}/build/app_host" --app gnn --messages "${messages}" --reference-only | sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p')
RUN_ID=${RUN_ID:-v2_gnn} MESSAGES=${messages} "${root}/tests/run_hardware_app.sh" gnn "${PORT:-7130}"
observed=$(sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p' "${root}/logs/${RUN_ID:-v2_gnn}/gnn.host.log")
test "${reference}" = "${observed}"
echo "PIMNIC V2 GNN PASS layer_digest=${observed} topology=8x8 axes=dim0,dim1"
