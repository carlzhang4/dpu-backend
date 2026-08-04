#ifndef PIMNIC_PARADIGM_COLLECTIVE_H
#define PIMNIC_PARADIGM_COLLECTIVE_H

#include <stdint.h>

#include "pimnic/abi/topology.h"
#include "pimnic/host/pe_set.h"

#ifdef __cplusplus
extern "C" {
#endif

int pimnic_collective_define(pimnic_pe_set_t *set,
			     const pimnic_collective_spec_t *spec,
			     uint32_t *collective_id);

#ifdef __cplusplus
}
#endif

#endif
