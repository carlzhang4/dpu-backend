#include "apps/common/app_harness.h"

PimnicAppPlan pimnic_kvstore_plan(const char *dpu_binary)
{
	PimnicAppPlan plan;
	plan.nr_pes = 64;
	plan.topology.nr_dims = 1;
	plan.topology.dims[0] = 64;
	plan.dpu_binary = dpu_binary;
	return plan;
}
