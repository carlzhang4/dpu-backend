#include "model.h"

std::vector<uint32_t> pimnic_select_run(const std::vector<uint32_t> &rows)
{
	std::vector<uint32_t> matches;
	for (uint32_t value : rows)
		if ((value & 1u) != 0)
			matches.push_back(value);
	return matches;
}
