#include <assert.h>

#include "apps/common/golden.h"

static std::vector<uint8_t> bytes(uint32_t pe, uint32_t n)
{
	std::vector<uint8_t> value(n);
	for (uint32_t i = 0; i < n; ++i)
		value[i] = static_cast<uint8_t>(pe * 13u + i);
	return value;
}

int main()
{
	pimnic_topology_t topology{};
	topology.nr_dims = 1;
	topology.dims[0] = 16;
	for (uint32_t primitive = PIMNIC_PRIM_BROADCAST;
	     primitive <= PIMNIC_PRIM_REDUCE; ++primitive)
		for (uint32_t writeback = 0; writeback <= 1; ++writeback) {
			pimnic_collective_spec_t spec{};
			spec.dim = 0;
			spec.prim = primitive;
			spec.root = 2;
			spec.writeback = writeback;
			spec.bytes_per_pe = 8;
			uint32_t input_bytes =
				primitive == PIMNIC_PRIM_SCATTER ? 128 : 8;
			PimnicGoldenBuffers inputs(16);
			for (uint32_t pe = 0; pe < 16; ++pe)
				inputs[pe] = bytes(pe, input_bytes);
			if (primitive == PIMNIC_PRIM_SCATTER) {
				inputs[spec.root].clear();
				for (uint32_t pe = 0; pe < 16; ++pe) {
					auto part = bytes(pe, 8);
					inputs[spec.root].insert(
						inputs[spec.root].end(),
						part.begin(), part.end());
				}
			}
			PimnicGoldenOutputs outputs;
			assert(pimnic_golden_collective(
				       topology, spec, inputs, &outputs) == 0);
			for (uint32_t pe = 0; pe < 16; ++pe) {
				bool target =
					primitive == PIMNIC_PRIM_BROADCAST ||
					primitive == PIMNIC_PRIM_SCATTER ||
					writeback != 0 || pe == spec.root;
				assert(outputs[pe].has_value() == target);
				if (!target)
					continue;
				if (primitive == PIMNIC_PRIM_BROADCAST)
					assert(*outputs[pe] ==
					       bytes(spec.root, 8));
				else if (primitive ==
					 PIMNIC_PRIM_SCATTER)
					assert(*outputs[pe] == bytes(pe, 8));
				else {
					std::vector<uint8_t> aggregate;
					for (uint32_t source = 0; source < 16;
					     ++source) {
						auto part = bytes(source, 8);
						aggregate.insert(
							aggregate.end(),
							part.begin(), part.end());
					}
					assert(*outputs[pe] == aggregate);
				}
			}
		}
	return 0;
}
