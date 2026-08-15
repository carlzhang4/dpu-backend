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
	for (const auto &preload : plan.preloads) {
		if (preload.dims.size() != plan.topology.nr_dims ||
		    preload.data.empty())
			return -EINVAL;
		rc = pimnic_preload(set_,
			preload.symbol.empty() ? nullptr : preload.symbol.c_str(),
			preload.offset, preload.data.data(), preload.bytes_per_pe,
			preload.dims.data());
		if (rc != 0)
			return rc;
	}
	for (const auto &spec : plan.collectives) {
		uint32_t id = 0;
		rc = pimnic_collective_define(set_, &spec, &id);
		if (rc != 0)
			return rc;
		collective_ids_.push_back(id);
	}
	return 0;
}

int PimnicAppHarness::run(const pimnic_export_params &export_params,
			  const pimnic_session_params_t &session,
			  pimnic_runtime_result *result, int timeout_ms)
{
	if (set_ == nullptr || !booted_ || result == nullptr)
		return -EINVAL;
	pimnic_exporter_t *exporter = nullptr;
	int rc = pimnic_export_open(set_, &export_params, &exporter);
	pimnic_runtime_config config{};
	if (rc == 0)
		rc = pimnic_handoff_build_config(set_, exporter, &session, &config);
	if (rc == 0)
		rc = pimnic_handoff_start(exporter, &config);
	if (rc == 0)
		rc = pimnic_handoff_wait_result(exporter, result, timeout_ms);
	if (rc == 0)
		rc = pimnic_handoff_reclaim(set_);
	if (exporter != nullptr)
		pimnic_export_close(exporter);
	return rc;
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

void PimnicAppHarness::reset()
{
	if (set_ != nullptr) {
		pimnic_pe_set_free(set_);
		set_ = nullptr;
	}
	collective_ids_.clear();
	booted_ = false;
}

PimnicAppHarness::~PimnicAppHarness()
{
	reset();
}
