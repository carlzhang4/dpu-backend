#ifndef PIMNIC_ABI_TOPOLOGY_H
#define PIMNIC_ABI_TOPOLOGY_H

#include <stdint.h>

#include "pimnic/abi/version.h"

#define PIMNIC_MAX_DIMS 4u

enum pimnic_prim {
	PIMNIC_PRIM_BROADCAST = 1,
	PIMNIC_PRIM_SCATTER = 2,
	PIMNIC_PRIM_GATHER = 3,
	PIMNIC_PRIM_REDUCE = 4,
};

typedef struct pimnic_topology {
	uint32_t nr_dims;
	uint32_t dims[PIMNIC_MAX_DIMS];
} pimnic_topology_t;

typedef struct pimnic_collective_spec {
	uint32_t pe_set_id;
	uint32_t dim;
	uint32_t prim;
	uint32_t root;
	uint32_t writeback;
	uint32_t bytes_per_pe;
	uint32_t tag;
	uint32_t reserved;
} pimnic_collective_spec_t;

typedef struct pimnic_dim_op {
	uint32_t dim;
	uint32_t prim;
	uint32_t root;
	uint32_t reserved;
} pimnic_dim_op_t;

PIMNIC_STATIC_ASSERT(sizeof(pimnic_topology_t) == 20,
		     "pimnic_topology ABI size");
PIMNIC_STATIC_ASSERT(sizeof(pimnic_collective_spec_t) == 32,
		     "pimnic_collective_spec ABI size");
PIMNIC_STATIC_ASSERT(sizeof(pimnic_dim_op_t) == 16,
		     "pimnic_dim_op ABI size");

#endif
