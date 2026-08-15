#!/usr/bin/env bash
# Build the instrumented V2 reference copies of the frozen benchmarks by
# reusing the objects and link lines of the original CMake targets (the
# originals themselves are never rebuilt or modified).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
repo=${PIM_REPO:-$(cd "${root}/../.." && pwd)}
build="${root}/build"
mkdir -p "${build}"

build_one() {
	local source_file=$1 original_source=$2 target=$3 link_dir=$4 output=$5
	local object="${build}/$(basename "${output}").o"
	python3 - "$@" "${repo}" "${object}" <<-'EOF'
	import json, shlex, subprocess, sys
	source, original, target, link_dir, output, repo, obj = sys.argv[1:8]
	entries = json.load(open(f"{repo}/build/compile_commands.json"))
	entry = next(e for e in entries if e["file"].endswith(original))
	words = shlex.split(entry["command"])
	cut = words.index("-o")
	source_dir = "/".join((repo + "/" + original).split("/")[:-1])
	compile_cmd = words[:cut] + ["-I" + source_dir, "-o", obj, "-c", source]
	subprocess.run(compile_cmd, check=True, cwd=entry["directory"])
	link_words = shlex.split(
		open(f"{link_dir}/CMakeFiles/{target}.dir/link.txt").read())
	own_obj = f"CMakeFiles/{target}.dir/{original.split('/')[-1]}.o"
	link_cmd = [obj if w == own_obj else w for w in link_words]
	cut = link_cmd.index("-o")
	link_cmd[cut + 1] = output
	subprocess.run(link_cmd, check=True, cwd=link_dir)
	print(f"built {output}")
	EOF
}

build_one "${root}/v2ref/select_pim_ref.cpp" \
	"benchmarks/SEL/select_pim.cpp" select_pim \
	"${repo}/build/benchmarks/SEL" "${build}/select_pim_ref"
build_one "${root}/v2ref/kvstore_pim_multidpu_ref.cpp" \
	"benchmarks/kvstore/kvstore_pim_multidpu.cpp" kvstore_pim_multidpu \
	"${repo}/build/benchmarks/kvstore" "${build}/kvstore_pim_multidpu_ref"
echo "pimnic v2ref build PASS"
