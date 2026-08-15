#include <errno.h>

#include "golden.h"

int pimnic_golden_collective(const pimnic_topology_t &topology,
			     const pimnic_collective_spec_t &spec,
			     const PimnicGoldenBuffers &inputs,
			     PimnicGoldenOutputs *outputs)
{
	if (outputs == nullptr || topology.nr_dims == 0 ||
	    topology.nr_dims > PIMNIC_MAX_DIMS || spec.dim >= topology.nr_dims ||
	    spec.prim < PIMNIC_PRIM_BROADCAST ||
	    spec.prim > PIMNIC_PRIM_REDUCE || spec.bytes_per_pe == 0)
		return -EINVAL;
	uint64_t nr_pes = 1;
	for (uint32_t dim = 0; dim < topology.nr_dims; ++dim) {
		if (topology.dims[dim] == 0)
			return -EINVAL;
		nr_pes *= topology.dims[dim];
	}
	uint32_t dim_size = topology.dims[spec.dim];
	if (spec.root >= dim_size || inputs.size() != nr_pes)
		return -EINVAL;
	uint32_t input_bytes =
		spec.prim == PIMNIC_PRIM_SCATTER ?
			spec.bytes_per_pe * dim_size :
			spec.bytes_per_pe;
	for (const auto &input : inputs)
		if (input.size() != input_bytes)
			return -EINVAL;
	outputs->assign(static_cast<size_t>(nr_pes), std::nullopt);

	uint32_t stride = 1;
	for (uint32_t dim = 0; dim < spec.dim; ++dim)
		stride *= topology.dims[dim];
	for (uint32_t pe = 0; pe < nr_pes; ++pe) {
		uint32_t coord = (pe / stride) % dim_size;
		if (coord != 0)
			continue;
		uint32_t line_base = pe;
		std::vector<uint32_t> peers(dim_size);
		for (uint32_t i = 0; i < dim_size; ++i)
			peers[i] = line_base + i * stride;
		if (spec.prim == PIMNIC_PRIM_BROADCAST) {
			const auto &source = inputs[peers[spec.root]];
			for (uint32_t target : peers)
				(*outputs)[target] = source;
		} else if (spec.prim == PIMNIC_PRIM_SCATTER) {
			const auto &source = inputs[peers[spec.root]];
			for (uint32_t i = 0; i < dim_size; ++i) {
				auto first = source.begin() +
					     i * spec.bytes_per_pe;
				(*outputs)[peers[i]] = std::vector<uint8_t>(
					first, first + spec.bytes_per_pe);
			}
		} else {
			std::vector<uint8_t> aggregate;
			aggregate.reserve(spec.bytes_per_pe * dim_size);
			for (uint32_t source : peers)
				aggregate.insert(aggregate.end(),
						 inputs[source].begin(),
						 inputs[source].end());
			(*outputs)[peers[spec.root]] = aggregate;
			if (spec.writeback)
				for (uint32_t target : peers)
					(*outputs)[target] = aggregate;
		}
	}
	return 0;
}
