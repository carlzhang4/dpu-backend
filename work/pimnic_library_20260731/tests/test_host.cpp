#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <array>

#include "pimnic/host/handoff.h"
#include "pimnic/host/mailbox.h"
#include "pimnic/paradigm/collective.h"
#include "pimnic/paradigm/preload.h"

struct Mock {
	uint32_t nr_pes = 0;
	uint32_t collective_id = 0;
	uint32_t mailbox = 0;
	int preload_calls = 0;
	int handoff_calls = 0;
	int reclaim_calls = 0;
	bool group_active = false;
};

static int mock_alloc(void *user, uint32_t nr_pes, const char *, void **handle)
{
	auto *mock = static_cast<Mock *>(user);
	mock->nr_pes = nr_pes;
	*handle = mock;
	return 0;
}

static int mock_load(void *, void *, const char *) { return 0; }
static int mock_config(void *, void *, const char *, const uint32_t *, uint32_t)
{
	return 0;
}
static int mock_boot(void *, void *) { return 0; }
static int mock_stop(void *, void *, uint32_t) { return 0; }
static void mock_free(void *, void *) {}
static int mock_preload(void *user, void *, const char *, uint32_t,
			const void *, uint32_t, const pimnic_dim_op_t *,
			uint32_t)
{
	static_cast<Mock *>(user)->preload_calls++;
	return 0;
}
static int mock_define(void *user, void *,
		       const pimnic_collective_spec_t *, uint32_t id)
{
	static_cast<Mock *>(user)->collective_id = id;
	return 0;
}
static int mock_read(void *user, void *, uint32_t, const char *,
		     uint32_t *value)
{
	*value = static_cast<Mock *>(user)->mailbox;
	return 0;
}
static int mock_write(void *user, void *, uint32_t, const char *,
		      uint32_t value)
{
	static_cast<Mock *>(user)->mailbox = value;
	return 0;
}
static int mock_export_open(void *, void *, const void *, void **handle,
			    int *fd)
{
	*handle = reinterpret_cast<void *>(0x1234);
	*fd = 77;
	return 0;
}
static void mock_export_close(void *, void *) {}
static int mock_build_config(void *, void *, void *,
			     pimnic_runtime_config *config)
{
	config->nr_ranks = 1;
	config->ranks[0].nr_cis = 8;
	config->ranks[0].ci_mask = 0xff;
	return 0;
}
static int mock_handoff_start(void *user, void *,
			      const pimnic_runtime_config *)
{
	static_cast<Mock *>(user)->handoff_calls++;
	return 0;
}
static int mock_group_active(void *user, void *, uint32_t group, int active)
{
	assert(group == 2);
	static_cast<Mock *>(user)->group_active = active != 0;
	return 0;
}
static int mock_wait_result(void *, void *, pimnic_runtime_result *result,
			    int)
{
	memset(result, 0, sizeof(*result));
	result->magic = PIMNIC_ABI_MAGIC;
	result->version = PIMNIC_ABI_VERSION;
	result->status = PIMNIC_STATUS_OK;
	return 0;
}
static int mock_reclaim(void *user, void *)
{
	static_cast<Mock *>(user)->reclaim_calls++;
	return 0;
}

int main()
{
	Mock mock;
	pimnic_host_ops ops{};
	ops.alloc = mock_alloc;
	ops.load = mock_load;
	ops.config_u32 = mock_config;
	ops.boot = mock_boot;
	ops.stop = mock_stop;
	ops.free_set = mock_free;
	ops.preload = mock_preload;
	ops.collective_define = mock_define;
	ops.mailbox_read_u32 = mock_read;
	ops.mailbox_write_u32 = mock_write;
	ops.export_open = mock_export_open;
	ops.export_close = mock_export_close;
	ops.handoff_build_config = mock_build_config;
	ops.handoff_start = mock_handoff_start;
	ops.group_set_active = mock_group_active;
	ops.wait_result = mock_wait_result;
	ops.handoff_reclaim = mock_reclaim;
	pimnic_init_params_t params{&ops, &mock, "backend=hw"};
	pimnic_ctx_t *ctx = nullptr;
	assert(pimnic_init(&params, &ctx) == 0);

	pimnic_topology_t topology{};
	topology.nr_dims = 2;
	topology.dims[0] = 8;
	topology.dims[1] = 8;
	pimnic_pe_set_t *set = nullptr;
	assert(pimnic_alloc_PE(ctx, 64, &topology, &set) == 0);
	assert(mock.nr_pes == 64);
	assert(pimnic_pe_set_size(set) == 64);
	assert(pimnic_pe_set_load(set, "kernel") == 0);

	std::array<uint8_t, 64> preload{};
	pimnic_dim_op_t dims[2]{};
	dims[0].dim = 0;
	dims[0].prim = PIMNIC_PRIM_SCATTER;
	dims[1].dim = 1;
	dims[1].prim = PIMNIC_PRIM_BROADCAST;
	assert(pimnic_preload(set, "heap", 0, preload.data(), 8, dims) == 0);
	assert(mock.preload_calls == 1);

	pimnic_collective_spec_t spec{};
	spec.pe_set_id = pimnic_pe_set_id(set);
	spec.dim = 1;
	spec.prim = PIMNIC_PRIM_REDUCE;
	spec.root = 0;
	spec.writeback = 1;
	spec.bytes_per_pe = 64;
	spec.tag = 99;
	uint32_t collective_id = 0;
	assert(pimnic_collective_define(set, &spec, &collective_id) == 0);
	assert(collective_id == mock.collective_id);

	assert(pimnic_mailbox_write_u32(set, 3, "word", 0x12345678) == 0);
	uint32_t value = 0;
	assert(pimnic_mailbox_read_u32(set, 3, "word", &value) == 0);
	assert(value == 0x12345678);

	assert(pimnic_pe_set_boot(set) == 0);
	assert(pimnic_mailbox_read_u32(set, 3, "word", &value) == -EINVAL);
	pimnic_export_params export_params{"127.0.0.1", 6892, 0};
	pimnic_exporter_t *exporter = nullptr;
	assert(pimnic_export_open(set, &export_params, &exporter) == 0);
	assert(pimnic_export_fd(exporter) == 77);

	pimnic_session_params_t session{};
	session.poll_interval_us = 10;
	session.timeout_us = 1000;
	session.active_group_mask = 0xf;
	session.flags = 7;
	session.app_config[0] = 0x5a;
	pimnic_runtime_config config{};
	assert(pimnic_handoff_build_config(set, exporter, &session,
					   &config) == 0);
	assert(config.magic == PIMNIC_ABI_MAGIC);
	assert(config.version == PIMNIC_ABI_VERSION);
	assert(config.nr_ranks == 1 && config.nr_groups == 4);
	assert(config.app_config[0] == 0x5a);
	assert(pimnic_handoff_start(exporter, &config) == 0);
	assert(mock.handoff_calls == 1);
	assert(pimnic_group_set_active(exporter, 2, 1) == 0);
	assert(mock.group_active);
	pimnic_runtime_result result{};
	assert(pimnic_handoff_wait_result(exporter, &result, 1000) == 0);
	assert(result.status == PIMNIC_STATUS_OK);
	assert(pimnic_handoff_reclaim(set) == 0);
	assert(mock.reclaim_calls == 1);
	pimnic_export_close(exporter);
	assert(pimnic_pe_set_stop(set, 1000) == 0);
	pimnic_pe_set_free(set);
	pimnic_fini(ctx);
	return 0;
}
