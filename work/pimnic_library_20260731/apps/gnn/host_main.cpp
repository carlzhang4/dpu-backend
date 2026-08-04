#include "apps/common/app_harness.h"

PimnicAppPlan pimnic_gnn_plan(const char *dpu_binary)
{
	PimnicAppPlan plan;
	plan.nr_pes = 64;
	plan.topology.nr_dims = 2;
	plan.topology.dims[0] = 8;
	plan.topology.dims[1] = 8;
	plan.dpu_binary = dpu_binary;
	pimnic_collective_spec_t reduce{};
	reduce.dim = 0;
	reduce.prim = PIMNIC_PRIM_REDUCE;
	reduce.root = 0;
	reduce.writeback = 1;
	reduce.bytes_per_pe = 16;
	reduce.tag = 1;
	plan.collectives.push_back(reduce);
	pimnic_collective_spec_t gather = reduce;
	gather.dim = 1;
	gather.prim = PIMNIC_PRIM_GATHER;
	gather.tag = 2;
	plan.collectives.push_back(gather);
	return plan;
}
