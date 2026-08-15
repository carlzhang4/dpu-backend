#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build="${root}/build"
repo=${PIM_REPO:-$(cd "${root}/../.." && pwd)}
mkdir -p "${build}"

cxx=${CXX:-g++}
cc=${CC:-gcc}
dpu_cc=${DPU_CC:-dpu-upmem-dpurte-clang}
ar_tool=${AR:-ar}
dpu_ar=${DPU_AR:-llvm-ar}

common=(-I"${root}" -I"${root}/include" -I"${root}/paradigm/include")
cxx_flags=(-std=c++17 -O2 -g -Wall -Wextra -Werror "${common[@]}")

"${cxx}" "${cxx_flags[@]}" -c "${root}/host/pimnic_host.cpp" \
	-o "${build}/pimnic_host.o"

sdk_flags_file="${repo}/build/example/CMakeFiles/bf_pimnic_runtime_host.dir/flags.make"
if [[ ! -f "${sdk_flags_file}" ]]; then
	echo "missing SDK flags: ${sdk_flags_file}" >&2
	exit 1
fi
read -r -a sdk_includes <<<"$(sed -n 's/^CXX_INCLUDES = //p' "${sdk_flags_file}")"
hardware_flags=(
	-std=c++17 -O2 -g -Wall -Wextra
	"${common[@]}" "${sdk_includes[@]}"
	-I"${repo}/bfdma"
)
"${cxx}" "${hardware_flags[@]}" -c "${root}/host/ops_upmem.cpp" \
	-o "${build}/ops_upmem.o"
"${ar_tool}" rcs "${build}/libpimnic_host.a" \
	"${build}/pimnic_host.o" "${build}/ops_upmem.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/common/golden.cpp" \
	-o "${build}/golden.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/common/app_harness.cpp" \
	-o "${build}/app_harness.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/kvstore/host_main.cpp" \
	-o "${build}/kvstore_host_main.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/select/host_main.cpp" \
	-o "${build}/select_host_main.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/gnn/host_main.cpp" \
	-o "${build}/gnn_host_main.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/kvstore/model.cpp" \
	-o "${build}/kvstore_model.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/select/model.cpp" \
	-o "${build}/select_model.o"
"${cxx}" "${cxx_flags[@]}" -c "${root}/apps/gnn/model.cpp" \
	-o "${build}/gnn_model.o"
"${ar_tool}" rcs "${build}/libpimnic_apps.a" \
	"${build}/golden.o" "${build}/app_harness.o" \
	"${build}/kvstore_host_main.o" "${build}/select_host_main.o" \
	"${build}/gnn_host_main.o" "${build}/kvstore_model.o" \
	"${build}/select_model.o" "${build}/gnn_model.o"

"${cxx}" "${cxx_flags[@]}" "${root}/tests/test_abi.cpp" \
	-o "${build}/test_abi"
"${cxx}" "${cxx_flags[@]}" "${root}/tests/test_host.cpp" \
	"${build}/libpimnic_host.a" -o "${build}/test_host"
"${cxx}" "${cxx_flags[@]}" "${root}/tests/test_golden.cpp" \
	"${build}/libpimnic_apps.a" "${build}/libpimnic_host.a" \
	-o "${build}/test_golden"
"${cxx}" "${cxx_flags[@]}" "${root}/tests/test_apps.cpp" \
	"${build}/libpimnic_apps.a" "${build}/libpimnic_host.a" \
	-o "${build}/test_apps"
"${build}/test_abi"
"${build}/test_host"
"${build}/test_golden"
"${build}/test_apps"

bf_dma_objects=()
for source in dma_export connection_manager libr util devx_device devx_mr; do
	object="${build}/bfdma_${source}.o"
	"${cxx}" "${hardware_flags[@]}" -c "${repo}/bfdma/${source}.cpp" \
		-o "${object}"
	bf_dma_objects+=("${object}")
done
"${cxx}" "${hardware_flags[@]}" "${root}/examples/runtime_host.cpp" \
	"${build}/libpimnic_host.a" "${bf_dma_objects[@]}" \
	"${repo}/build/example/CMakeFiles/bf_pimnic_runtime_host.dir/__/ufi/src/ufi_ci.c.o" \
	-no-pie -o "${build}/runtime_host" \
	-L"${repo}/build/pimnic" -lpimnic \
	-L"${repo}/build/api" -ldpu -L"${repo}/build/hw" -ldpuhw \
	-L"${repo}/build/vpd" -ldpuvpd \
	-L"${repo}/build/verbose" -ldpuverbose \
	-L"${repo}/build/util" -lutils \
	-Wl,-rpath,"${repo}/build/pimnic:${repo}/build/api:${repo}/build/hw:${repo}/build/vpd:${repo}/build/verbose:${repo}/build/util" \
	-libverbs -lmlx5 -lnuma -lelf -lpython3.8 -ludev \
	-ldl -lpthread -lm
"${cxx}" "${hardware_flags[@]}" "${root}/examples/collective_host.cpp" \
	"${build}/libpimnic_host.a" "${bf_dma_objects[@]}" \
	"${repo}/build/example/CMakeFiles/bf_pimnic_runtime_host.dir/__/ufi/src/ufi_ci.c.o" \
	-no-pie -o "${build}/collective_host" \
	-L"${repo}/build/pimnic" -lpimnic \
	-L"${repo}/build/api" -ldpu -L"${repo}/build/hw" -ldpuhw \
	-L"${repo}/build/vpd" -ldpuvpd \
	-L"${repo}/build/verbose" -ldpuverbose \
	-L"${repo}/build/util" -lutils \
	-Wl,-rpath,"${repo}/build/pimnic:${repo}/build/api:${repo}/build/hw:${repo}/build/vpd:${repo}/build/verbose:${repo}/build/util" \
	-libverbs -lmlx5 -lnuma -lelf -lpython3.8 -ludev \
	-ldl -lpthread -lm
"${cxx}" "${hardware_flags[@]}" "${root}/examples/app_host.cpp" \
	"${build}/libpimnic_apps.a" "${build}/libpimnic_host.a" \
	"${bf_dma_objects[@]}" \
	"${repo}/build/example/CMakeFiles/bf_pimnic_runtime_host.dir/__/ufi/src/ufi_ci.c.o" \
	-no-pie -o "${build}/app_host" \
	-L"${repo}/build/pimnic" -lpimnic \
	-L"${repo}/build/api" -ldpu -L"${repo}/build/hw" -ldpuhw \
	-L"${repo}/build/vpd" -ldpuvpd \
	-L"${repo}/build/verbose" -ldpuverbose \
	-L"${repo}/build/util" -lutils \
	-Wl,-rpath,"${repo}/build/pimnic:${repo}/build/api:${repo}/build/hw:${repo}/build/vpd:${repo}/build/verbose:${repo}/build/util" \
	-libverbs -lmlx5 -lnuma -lelf -lpython3.8 -ludev \
	-ldl -lpthread -lm

"${dpu_cc}" -std=c11 -O2 -g "${common[@]}" \
	-c "${root}/dpu/pe.c" -o "${build}/pe.o"
"${dpu_cc}" -std=c11 -O2 -g "${common[@]}" \
	-c "${root}/paradigm/pe_paradigm.c" \
	-o "${build}/pe_paradigm.o"
"${dpu_ar}" rcs "${build}/libpimnic_dpu.a" \
	"${build}/pe.o" "${build}/pe_paradigm.o"
"${dpu_cc}" -std=c11 -O2 -g "${common[@]}" \
	"${root}/examples/runtime_pe.c" "${build}/libpimnic_dpu.a" \
	-o "${build}/runtime_pe"
"${dpu_cc}" -std=c11 -O2 -g "${common[@]}" \
	"${root}/examples/paradigm_echo_pe.c" \
	"${build}/libpimnic_dpu.a" -o "${build}/paradigm_echo_pe"
"${dpu_cc}" -std=c11 -O2 -g "${common[@]}" \
	"${root}/examples/collective_pe.c" \
	"${build}/libpimnic_dpu.a" -o "${build}/collective_pe"
for app in kvstore select gnn; do
	link_flags=()
	if [[ "${app}" == gnn ]]; then
		link_flags+=("-Wl,-Map=${build}/gnn_pe.map")
	fi
	"${dpu_cc}" -std=c11 -O2 -g "${common[@]}" \
		"${link_flags[@]}" \
		"${root}/apps/${app}/pe_kernel.c" \
		"${build}/libpimnic_dpu.a" \
		-o "${build}/${app}_pe"
done

nm -S "${build}/runtime_pe" |
	grep -E ' (rx_desc|tx_desc|rx_data|tx_data|pe_pub|nic_pub|gate_command|gate_ack|stop)$'
echo "pimnic PIM1 build PASS"
