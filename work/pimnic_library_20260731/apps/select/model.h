#ifndef PIMNIC_APPS_SELECT_MODEL_H
#define PIMNIC_APPS_SELECT_MODEL_H

#include <array>
#include <stdint.h>
#include <vector>

#define PIMNIC_SELECT_MAX_MATCHES 31u

struct PimnicSelectResult {
	uint32_t count = 0;
	std::array<uint32_t, PIMNIC_SELECT_MAX_MATCHES> values{};
};

PimnicSelectResult pimnic_select_run(const std::vector<uint32_t> &rows,
				     uint32_t lower, uint32_t upper);

#endif
