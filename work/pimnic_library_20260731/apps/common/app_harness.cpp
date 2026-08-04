#include <errno.h>

#include "app_harness.h"

int PimnicAppHarness::deploy(pimnic_ctx_t *ctx, const PimnicAppPlan &plan)
{
	if (ctx == nullptr || set_ != nullptr || plan.dpu_binary.empty())
		return -EINVAL;
	int rc = pimnic_alloc_PE(ctx, plan.nr_pes, &plan.topology, &set_);
	if (rc != 0)
		return rc;
	rc = pimnic_pe_set_load(set_, plan.dpu_binary.c_str());
	if (rc != 0)
		return rc;
	for (const auto &spec : plan.collectives) {
		uint32_t id = 0;
		rc = pimnic_collective_define(set_, &spec, &id);
		if (rc != 0)
			return rc;
		collective_ids_.push_back(id);
	}
	return 0;
}

int PimnicAppHarness::boot()
{
	if (set_ == nullptr || booted_)
		return -EINVAL;
	int rc = pimnic_pe_set_boot(set_);
	if (rc == 0)
		booted_ = true;
	return rc;
}

int PimnicAppHarness::stop(uint32_t timeout_us)
{
	if (set_ == nullptr || !booted_)
		return -EINVAL;
	int rc = pimnic_pe_set_stop(set_, timeout_us);
	if (rc == 0)
		booted_ = false;
	return rc;
}

PimnicAppHarness::~PimnicAppHarness()
{
	if (set_ != nullptr)
		pimnic_pe_set_free(set_);
}
