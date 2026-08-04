#include <errno.h>

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "pimnic/host/export.h"
#include "pimnic/host/handoff.h"
#include "pimnic/host/mailbox.h"
#include "pimnic/paradigm/collective.h"
#include "pimnic/paradigm/preload.h"

struct pimnic_ctx {
	pimnic_host_ops ops{};
	void *user = nullptr;
	std::string profile;
	uint32_t next_set_id = 1;
};

struct pimnic_pe_set {
	pimnic_ctx *ctx = nullptr;
	void *platform_set = nullptr;
	uint32_t id = 0;
	uint32_t nr_pes = 0;
	pimnic_topology_t topology{};
	uint32_t next_collective_id = 1;
	bool loaded = false;
	bool booted = false;
};

struct pimnic_exporter {
	pimnic_pe_set *set = nullptr;
	void *platform_exporter = nullptr;
	int control_fd = -1;
};

static pimnic_ctx *default_ctx;

static int validate_topology(uint32_t nr_pes,
			     const pimnic_topology_t *topology)
{
	if (topology == nullptr || topology->nr_dims == 0 ||
	    topology->nr_dims > PIMNIC_MAX_DIMS)
		return -EINVAL;
	uint64_t product = 1;
	for (uint32_t dim = 0; dim < topology->nr_dims; ++dim) {
		if (topology->dims[dim] == 0)
			return -EINVAL;
		product *= topology->dims[dim];
	}
	return product == nr_pes ? 0 : -EINVAL;
}

extern "C" int pimnic_init(const pimnic_init_params_t *params,
			    pimnic_ctx_t **out)
{
	if (params == nullptr || params->ops == nullptr || out == nullptr ||
	    params->ops->alloc == nullptr || params->ops->free_set == nullptr)
		return -EINVAL;
	auto *ctx = new (std::nothrow) pimnic_ctx;
	if (ctx == nullptr)
		return -ENOMEM;
	ctx->ops = *params->ops;
	ctx->user = params->user;
	if (params->profile != nullptr)
		ctx->profile = params->profile;
	*out = ctx;
	if (default_ctx == nullptr)
		default_ctx = ctx;
	return 0;
}

extern "C" void pimnic_fini(pimnic_ctx_t *ctx)
{
	if (default_ctx == ctx)
		default_ctx = nullptr;
	delete ctx;
}

extern "C" int pimnic_alloc_PE(pimnic_ctx_t *ctx, uint32_t nr_pes,
			       const pimnic_topology_t *topology,
			       pimnic_pe_set_t **out)
{
	if (ctx == nullptr || out == nullptr || nr_pes == 0 ||
	    (nr_pes % 64u) != 0)
		return -EINVAL;
	int rc = validate_topology(nr_pes, topology);
	if (rc != 0)
		return rc;
	auto *set = new (std::nothrow) pimnic_pe_set;
	if (set == nullptr)
		return -ENOMEM;
	set->ctx = ctx;
	set->id = ctx->next_set_id++;
	set->nr_pes = nr_pes;
	set->topology = *topology;
	rc = ctx->ops.alloc(ctx->user, nr_pes, ctx->profile.c_str(),
			    &set->platform_set);
	if (rc != 0) {
		delete set;
		return rc;
	}
	*out = set;
	return 0;
}

extern "C" int pimnic_pe_set_alloc(uint32_t nr_pes, const char *profile,
				   pimnic_pe_set_t **out)
{
	if (default_ctx == nullptr)
		return -ENODEV;
	pimnic_topology_t topology{};
	topology.nr_dims = 1;
	topology.dims[0] = nr_pes;
	std::string saved = default_ctx->profile;
	if (profile != nullptr)
		default_ctx->profile = profile;
	int rc = pimnic_alloc_PE(default_ctx, nr_pes, &topology, out);
	default_ctx->profile = saved;
	return rc;
}

extern "C" int pimnic_pe_set_load(pimnic_pe_set_t *set,
				  const char *dpu_binary)
{
	if (set == nullptr || dpu_binary == nullptr ||
	    set->ctx->ops.load == nullptr)
		return -EINVAL;
	int rc = set->ctx->ops.load(set->ctx->user, set->platform_set,
				    dpu_binary);
	if (rc == 0)
		set->loaded = true;
	return rc;
}

extern "C" int pimnic_pe_set_config_u32(
	pimnic_pe_set_t *set, const char *symbol,
	const uint32_t *per_pe_values)
{
	if (set == nullptr || symbol == nullptr || per_pe_values == nullptr ||
	    set->ctx->ops.config_u32 == nullptr || set->booted)
		return -EINVAL;
	return set->ctx->ops.config_u32(set->ctx->user, set->platform_set,
					symbol, per_pe_values, set->nr_pes);
}

extern "C" int pimnic_pe_set_boot(pimnic_pe_set_t *set)
{
	if (set == nullptr || !set->loaded || set->booted ||
	    set->ctx->ops.boot == nullptr)
		return -EINVAL;
	int rc = set->ctx->ops.boot(set->ctx->user, set->platform_set);
	if (rc == 0)
		set->booted = true;
	return rc;
}

extern "C" int pimnic_pe_set_stop(pimnic_pe_set_t *set,
				  uint32_t timeout_us)
{
	if (set == nullptr || !set->booted || set->ctx->ops.stop == nullptr)
		return -EINVAL;
	int rc = set->ctx->ops.stop(set->ctx->user, set->platform_set,
				    timeout_us);
	if (rc == 0)
		set->booted = false;
	return rc;
}

extern "C" void pimnic_pe_set_free(pimnic_pe_set_t *set)
{
	if (set == nullptr)
		return;
	if (set->platform_set != nullptr)
		set->ctx->ops.free_set(set->ctx->user, set->platform_set);
	delete set;
}

extern "C" uint32_t pimnic_pe_set_id(const pimnic_pe_set_t *set)
{
	return set == nullptr ? 0 : set->id;
}

extern "C" uint32_t pimnic_pe_set_size(const pimnic_pe_set_t *set)
{
	return set == nullptr ? 0 : set->nr_pes;
}

extern "C" const pimnic_topology_t *
pimnic_pe_set_topology(const pimnic_pe_set_t *set)
{
	return set == nullptr ? nullptr : &set->topology;
}

extern "C" int pimnic_collective_define(
	pimnic_pe_set_t *set, const pimnic_collective_spec_t *spec,
	uint32_t *collective_id)
{
	if (set == nullptr || spec == nullptr || collective_id == nullptr ||
	    spec->dim >= set->topology.nr_dims ||
	    spec->root >= set->topology.dims[spec->dim] ||
	    spec->prim < PIMNIC_PRIM_BROADCAST ||
	    spec->prim > PIMNIC_PRIM_REDUCE ||
	    spec->bytes_per_pe == 0 || (spec->bytes_per_pe & 7u) != 0 ||
	    spec->bytes_per_pe + sizeof(pimnic_message_header) >
		    PIMNIC_PAYLOAD_MAX)
		return -EINVAL;
	uint32_t id = set->next_collective_id++;
	if (set->ctx->ops.collective_define != nullptr) {
		int rc = set->ctx->ops.collective_define(
			set->ctx->user, set->platform_set, spec, id);
		if (rc != 0)
			return rc;
	}
	*collective_id = id;
	return 0;
}

extern "C" int pimnic_preload(pimnic_pe_set_t *set, const char *symbol,
			      uint32_t offset, const void *host_src,
			      uint32_t bytes_per_pe,
			      const pimnic_dim_op_t dims[])
{
	if (set == nullptr || host_src == nullptr || bytes_per_pe == 0 ||
	    set->ctx->ops.preload == nullptr || set->booted)
		return -EINVAL;
	return set->ctx->ops.preload(
		set->ctx->user, set->platform_set, symbol, offset, host_src,
		bytes_per_pe, dims, set->topology.nr_dims);
}

extern "C" int pimnic_mailbox_read_u32(pimnic_pe_set_t *set,
				       uint32_t pe, const char *symbol,
				       uint32_t *value)
{
	if (set == nullptr || pe >= set->nr_pes || symbol == nullptr ||
	    value == nullptr || set->booted ||
	    set->ctx->ops.mailbox_read_u32 == nullptr)
		return -EINVAL;
	return set->ctx->ops.mailbox_read_u32(
		set->ctx->user, set->platform_set, pe, symbol, value);
}

extern "C" int pimnic_mailbox_write_u32(pimnic_pe_set_t *set,
					uint32_t pe, const char *symbol,
					uint32_t value)
{
	if (set == nullptr || pe >= set->nr_pes || symbol == nullptr ||
	    set->booted || set->ctx->ops.mailbox_write_u32 == nullptr)
		return -EINVAL;
	return set->ctx->ops.mailbox_write_u32(
		set->ctx->user, set->platform_set, pe, symbol, value);
}

extern "C" int pimnic_export_open(
	pimnic_pe_set_t *set, const struct pimnic_export_params *params,
	pimnic_exporter_t **out)
{
	if (set == nullptr || params == nullptr || out == nullptr ||
	    set->ctx->ops.export_open == nullptr)
		return -EINVAL;
	auto *exporter = new (std::nothrow) pimnic_exporter;
	if (exporter == nullptr)
		return -ENOMEM;
	exporter->set = set;
	int rc = set->ctx->ops.export_open(
		set->ctx->user, set->platform_set, params,
		&exporter->platform_exporter, &exporter->control_fd);
	if (rc != 0) {
		delete exporter;
		return rc;
	}
	*out = exporter;
	return 0;
}

extern "C" int pimnic_export_fd(const pimnic_exporter_t *exporter)
{
	return exporter == nullptr ? -1 : exporter->control_fd;
}

extern "C" void pimnic_export_close(pimnic_exporter_t *exporter)
{
	if (exporter == nullptr)
		return;
	if (exporter->platform_exporter != nullptr &&
	    exporter->set->ctx->ops.export_close != nullptr)
		exporter->set->ctx->ops.export_close(
			exporter->set->ctx->user, exporter->platform_exporter);
	delete exporter;
}

extern "C" int pimnic_handoff_build_config(
	pimnic_pe_set_t *set, pimnic_exporter_t *exporter,
	const pimnic_session_params_t *params,
	struct pimnic_runtime_config *config)
{
	if (set == nullptr || exporter == nullptr || params == nullptr ||
	    config == nullptr || exporter->set != set ||
	    params->timeout_us == 0 || params->active_group_mask == 0 ||
	    set->ctx->ops.handoff_build_config == nullptr)
		return -EINVAL;

	std::memset(config, 0, sizeof(*config));
	config->magic = PIMNIC_ABI_MAGIC;
	config->version = PIMNIC_ABI_VERSION;
	config->nr_groups = set->nr_pes / PIMNIC_PE_GROUP_SIZE;
	config->poll_interval_us = params->poll_interval_us;
	config->timeout_us = params->timeout_us;
	config->active_group_mask = params->active_group_mask;
	config->flags = params->flags;
	std::memcpy(config->app_config, params->app_config,
		    sizeof(config->app_config));

	int rc = set->ctx->ops.handoff_build_config(
		set->ctx->user, set->platform_set,
		exporter->platform_exporter, config);
	if (rc != 0)
		return rc;
	uint32_t expected_ranks = set->nr_pes / 64u;
	uint32_t valid_groups =
		config->nr_groups == 32u ? UINT32_MAX :
					 ((1u << config->nr_groups) - 1u);
	if (config->nr_ranks != expected_ranks ||
	    config->nr_groups !=
		    config->nr_ranks * PIMNIC_GROUPS_PER_RANK ||
	    (config->active_group_mask & ~valid_groups) != 0)
		return -EPROTO;
	return 0;
}

extern "C" int pimnic_handoff_start(
	pimnic_exporter_t *exporter,
	const struct pimnic_runtime_config *config)
{
	if (exporter == nullptr || config == nullptr ||
	    config->magic != PIMNIC_ABI_MAGIC ||
	    config->version != PIMNIC_ABI_VERSION ||
	    exporter->set->ctx->ops.handoff_start == nullptr)
		return -EINVAL;
	return exporter->set->ctx->ops.handoff_start(
		exporter->set->ctx->user, exporter->platform_exporter, config);
}

extern "C" int pimnic_group_set_active(pimnic_exporter_t *exporter,
				       uint32_t group, int active)
{
	if (exporter == nullptr ||
	    exporter->set->ctx->ops.group_set_active == nullptr)
		return -EINVAL;
	return exporter->set->ctx->ops.group_set_active(
		exporter->set->ctx->user, exporter->platform_exporter, group,
		active != 0);
}

extern "C" int pimnic_handoff_wait_result(
	pimnic_exporter_t *exporter, struct pimnic_runtime_result *result,
	int timeout_ms)
{
	if (exporter == nullptr || result == nullptr ||
	    exporter->set->ctx->ops.wait_result == nullptr)
		return -EINVAL;
	int rc = exporter->set->ctx->ops.wait_result(
		exporter->set->ctx->user, exporter->platform_exporter, result,
		timeout_ms);
	if (rc == 0 &&
	    (result->magic != PIMNIC_ABI_MAGIC ||
	     result->version != PIMNIC_ABI_VERSION))
		return -EPROTO;
	return rc;
}

extern "C" int pimnic_handoff_reclaim(pimnic_pe_set_t *set)
{
	if (set == nullptr || set->ctx->ops.handoff_reclaim == nullptr)
		return -EINVAL;
	return set->ctx->ops.handoff_reclaim(set->ctx->user,
					     set->platform_set);
}
