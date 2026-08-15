#ifndef PIMNIC_APPS_SELECT_MODEL_H
#define PIMNIC_APPS_SELECT_MODEL_H

#include <stdint.h>
#include <vector>

/* CPU model of the original benchmarks/SEL predicate
 * (support/common.h pred + select_device_tasklets_parallel.c): keep the
 * values for which !pred(x), i.e. the odd values, in row order. */
std::vector<uint32_t> pimnic_select_run(const std::vector<uint32_t> &rows);

#endif
