#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bf3_root=${BF3_WORK_ROOT:-/home/pimnic/ziyu/nfs/libr/work/pimnic_library_20260731}
src="${root}/include/pimnic/abi"
dst="${bf3_root}/include/pimnic/abi"

mkdir -p "${dst}"
for header in version.h ring.h topology.h wire.h; do
	cp "${src}/${header}" "${dst}/${header}"
done

src_hash=$(cd "${src}" && sha256sum version.h ring.h topology.h wire.h)
dst_hash=$(cd "${dst}" && sha256sum version.h ring.h topology.h wire.h)
if [[ "${src_hash}" != "${dst_hash}" ]]; then
	echo "PIMNIC ABI mirror mismatch" >&2
	exit 1
fi
echo "PIMNIC ABI mirror PASS"
