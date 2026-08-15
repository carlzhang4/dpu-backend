#ifndef PIMNIC_APPS_APP_HARNESS_H
#define PIMNIC_APPS_APP_HARNESS_H

#include <string>
#include <vector>

#include "pimnic/host/pe_set.h"
#include "pimnic/host/handoff.h"
#include "pimnic/paradigm/collective.h"
#include "pimnic/paradigm/preload.h"

struct PimnicAppPreload {
	std::string symbol;
	uint32_t offset = 0;
	uint32_t bytes_per_pe = 0;
	std::vector<uint8_t> data;
	std::vector<pimnic_dim_op_t> dims;
};

struct PimnicAppPlan {
	uint32_t nr_pes = 0;
	pimnic_topology_t topology{};
	std::string dpu_binary;
	std::vector<PimnicAppPreload> preloads;
	std::vector<pimnic_collective_spec_t> collectives;
};

class PimnicAppHarness {
public:
	int deploy(pimnic_ctx_t *ctx, const PimnicAppPlan &plan);
	int boot();
	int run(const pimnic_export_params &export_params,
		const pimnic_session_params_t &session,
		pimnic_runtime_result *result, int timeout_ms = -1);
	int stop(uint32_t timeout_us);
	void reset();
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
