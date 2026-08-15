#ifndef PIMNIC_APPS_GOLDEN_H
#define PIMNIC_APPS_GOLDEN_H

#include <optional>
#include <stdint.h>
#include <vector>

#include "pimnic/abi/topology.h"

using PimnicGoldenBuffers = std::vector<std::vector<uint8_t>>;
using PimnicGoldenOutputs =
	std::vector<std::optional<std::vector<uint8_t>>>;

int pimnic_golden_collective(const pimnic_topology_t &topology,
			     const pimnic_collective_spec_t &spec,
			     const PimnicGoldenBuffers &inputs,
			     PimnicGoldenOutputs *outputs);

#endif
