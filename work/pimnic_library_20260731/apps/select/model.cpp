#include "model.h"

PimnicSelectResult pimnic_select_run(const std::vector<uint32_t> &rows,
				     uint32_t lower, uint32_t upper)
{
	PimnicSelectResult result;
	for (uint32_t value : rows)
		if (value >= lower && value <= upper &&
		    result.count < result.values.size())
			result.values[result.count++] = value;
	return result;
}
