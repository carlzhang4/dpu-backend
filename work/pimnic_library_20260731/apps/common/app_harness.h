#ifndef PIMNIC_APPS_APP_HARNESS_H
#define PIMNIC_APPS_APP_HARNESS_H

#include <string>
#include <vector>

#include "pimnic/host/pe_set.h"
#include "pimnic/paradigm/collective.h"

struct PimnicAppPlan {
	uint32_t nr_pes = 0;
	pimnic_topology_t topology{};
	std::string dpu_binary;
	std::vector<pimnic_collective_spec_t> collectives;
};

class PimnicAppHarness {
public:
	int deploy(pimnic_ctx_t *ctx, const PimnicAppPlan &plan);
	int boot();
	int stop(uint32_t timeout_us);
	pimnic_pe_set_t *set() const { return set_; }
	const std::vector<uint32_t> &collective_ids() const
	{
		return collective_ids_;
	}
	~PimnicAppHarness();

private:
	pimnic_pe_set_t *set_ = nullptr;
	std::vector<uint32_t> collective_ids_;
	bool booted_ = false;
};

#endif
