#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
messages=${MESSAGES:-8}
reference=$("${root}/build/app_host" --app kvstore --messages "${messages}" --reference-only | sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p')
RUN_ID=${RUN_ID:-v2_kvstore} MESSAGES=${messages} "${root}/tests/run_hardware_app.sh" kvstore "${PORT:-7110}"
observed=$(sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p' "${root}/logs/${RUN_ID:-v2_kvstore}/kvstore.host.log")
test "${reference}" = "${observed}"
echo "PIMNIC V2 KVStore PASS byte_digest=${observed}"
