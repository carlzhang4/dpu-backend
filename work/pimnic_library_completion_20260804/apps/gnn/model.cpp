#include "model.h"

std::vector<int32_t> pimnic_gnn_layer(const std::vector<int32_t> &input,
				      uint32_t cycle)
{
	std::vector<int32_t> output(input.size());
	for (size_t i = 0; i < input.size(); ++i)
		output[i] = input[i] + static_cast<int32_t>(cycle);
	return output;
}
