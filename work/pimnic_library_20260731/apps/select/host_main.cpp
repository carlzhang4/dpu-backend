#include "apps/common/app_harness.h"

PimnicAppPlan pimnic_select_plan(const char *dpu_binary)
{
	PimnicAppPlan plan;
	plan.nr_pes = 64;
	plan.topology.nr_dims = 1;
	plan.topology.dims[0] = 64;
	plan.dpu_binary = dpu_binary;
	pimnic_collective_spec_t query{};
	query.dim = 0;
	query.prim = PIMNIC_PRIM_BROADCAST;
	query.root = 0;
	query.bytes_per_pe = 8;
	query.tag = 1;
	plan.collectives.push_back(query);
	return plan;
}
