#ifndef PIMNIC_PARADIGM_PRELOAD_H
#define PIMNIC_PARADIGM_PRELOAD_H

#include <stdint.h>

#include "pimnic/abi/topology.h"
#include "pimnic/host/pe_set.h"

#ifdef __cplusplus
extern "C" {
#endif

int pimnic_preload(pimnic_pe_set_t *set, const char *symbol, uint32_t offset,
		   const void *host_src, uint32_t bytes_per_pe,
		   const pimnic_dim_op_t dims[]);

#ifdef __cplusplus
}
#endif

#endif
