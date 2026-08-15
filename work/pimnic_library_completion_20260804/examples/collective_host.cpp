#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "pimnic/host/handoff.h"
#include "pimnic/host/mailbox.h"
#include "pimnic/host/ops_upmem.h"
#include "pimnic/paradigm/collective.h"

struct Options {
	uint32_t nr_pes = 64, prim = PIMNIC_PRIM_BROADCAST, root = 0;
	uint32_t writeback = 0, bytes = 8, rounds = 3, dim = 0;
	uint32_t dim0 = 64, dim1 = 0, timeout_us = 500000;
	int port = 6900;
	std::string binary = "./build/collective_pe";
};

static bool number(const char *text, uint32_t *out)
{
	char *end = nullptr;
	unsigned long value = strtoul(text, &end, 0);
	if (end == text || *end != '\0' || value > UINT32_MAX) return false;
	*out = static_cast<uint32_t>(value);
	return true;
}

static bool parse(int argc, char **argv, Options *o)
{
	for (int i = 1; i < argc; ++i) {
		if (i + 1 == argc) return false;
		std::string f = argv[i++];
		const char *v = argv[i];
		uint32_t *target = nullptr;
		if (f == "--num-dpus") target = &o->nr_pes;
		else if (f == "--prim") target = &o->prim;
		else if (f == "--root") target = &o->root;
		else if (f == "--writeback") target = &o->writeback;
		else if (f == "--bytes") target = &o->bytes;
		else if (f == "--rounds") target = &o->rounds;
		else if (f == "--dim") target = &o->dim;
		else if (f == "--dim0") target = &o->dim0;
		else if (f == "--dim1") target = &o->dim1;
		else if (f == "--timeout-us") target = &o->timeout_us;
		else if (f == "--port") { o->port = atoi(v); continue; }
		else if (f == "--binary") { o->binary = v; continue; }
		else return false;
		if (!number(v, target)) return false;
	}
	return true;
}

static int configure_all(pimnic_pe_set_t *set, const char *symbol,
			 uint32_t nr_pes, uint32_t value)
{
	std::vector<uint32_t> values(nr_pes, value);
	return pimnic_pe_set_config_u32(set, symbol, values.data());
}

int main(int argc, char **argv)
{
	Options o;
	uint32_t topology_pes = o.dim1 == 0 ? o.dim0 : o.dim0 * o.dim1;
	if (!parse(argc, argv, &o)) return 2;
	topology_pes = o.dim1 == 0 ? o.dim0 : o.dim0 * o.dim1;
	uint32_t dim_size = o.dim == 0 ? o.dim0 : o.dim1;
	uint32_t input = o.prim == PIMNIC_PRIM_SCATTER ? o.bytes * dim_size :
							  o.bytes;
	uint32_t output = (o.prim == PIMNIC_PRIM_GATHER ||
			   o.prim == PIMNIC_PRIM_REDUCE) ?
				  o.bytes * dim_size : o.bytes;
	if (o.nr_pes != topology_pes || o.nr_pes % 64 != 0 || o.dim > 1 ||
	    dim_size == 0 || o.root >= dim_size || (o.bytes & 7u) != 0 ||
	    input + sizeof(pimnic_message_header) > PIMNIC_PAYLOAD_MAX ||
	    output + sizeof(pimnic_message_header) > PIMNIC_PAYLOAD_MAX)
		return 2;

	pimnic_upmem_params_t upmem{ "mlx5_0", 0, o.port };
	pimnic_init_params_t init{ pimnic_upmem_host_ops(), &upmem,
				   "backend=hw" };
	pimnic_ctx_t *ctx = nullptr;
	pimnic_pe_set_t *set = nullptr;
	pimnic_exporter_t *exporter = nullptr;
	int rc = pimnic_init(&init, &ctx);
	pimnic_topology_t topology{};
	topology.nr_dims = o.dim1 == 0 ? 1 : 2;
	topology.dims[0] = o.dim0;
	topology.dims[1] = o.dim1;
	if (rc == 0) rc = pimnic_alloc_PE(ctx, o.nr_pes, &topology, &set);
	if (rc == 0) rc = pimnic_pe_set_load(set, o.binary.c_str());
	pimnic_collective_spec_t spec{};
	spec.pe_set_id = pimnic_pe_set_id(set);
	spec.dim = o.dim;
	spec.prim = o.prim;
	spec.root = o.root;
	spec.writeback = o.writeback;
	spec.bytes_per_pe = o.bytes;
	spec.tag = 7;
	uint32_t collective_id = 0;
	if (rc == 0) rc = pimnic_collective_define(set, &spec, &collective_id);
	struct Pair { const char *name; uint32_t value; } values[] = {
		{ "collective_id", collective_id }, { "collective_rounds", o.rounds },
		{ "collective_prim", o.prim }, { "collective_root", o.root },
		{ "collective_writeback", o.writeback },
		{ "collective_bytes_per_pe", o.bytes }, { "collective_dim", o.dim },
		{ "collective_dim0", o.dim0 }, { "collective_dim1", o.dim1 },
	};
	for (const Pair &value : values)
		if (rc == 0) rc = configure_all(set, value.name, o.nr_pes, value.value);
	pimnic_export_params export_params{ nullptr, static_cast<uint16_t>(o.port), 0 };
	if (rc == 0) rc = pimnic_export_open(set, &export_params, &exporter);
	if (rc == 0) rc = pimnic_pe_set_boot(set);

	pimnic_session_params_t session{};
	session.poll_interval_us = 10;
	session.timeout_us = o.timeout_us;
	uint32_t groups = o.nr_pes / PIMNIC_PE_GROUP_SIZE;
	session.active_group_mask = groups == 32 ? UINT32_MAX : (1u << groups) - 1u;
	pimnic_app_runtime_config app{};
	app.magic = PIMNIC_APP_MAGIC;
	app.kind = PIMNIC_APP_COLLECTIVE;
	app.mode = PIMNIC_APP_MODE_ECHO;
	app.payload_bytes = sizeof(pimnic_message_header) +
				    (input > output ? input : output);
	app.messages_per_group = o.rounds;
	app.batch_size = 1;
	app.topology_dims = o.dim0 | (o.dim1 << 16);
	memcpy(session.app_config, &app, sizeof(app));
	pimnic_runtime_config config{};
	if (rc == 0) rc = pimnic_handoff_build_config(set, exporter, &session, &config);
	if (rc == 0) rc = pimnic_handoff_start(exporter, &config);
	pimnic_runtime_result result{};
	if (rc == 0) rc = pimnic_handoff_wait_result(exporter, &result, -1);
	if (rc == 0) rc = pimnic_handoff_reclaim(set);
	if (set != nullptr && pimnic_pe_set_stop(set, 5000000) != 0 && rc == 0)
		rc = -1;
	uint32_t failures = 0;
	if (rc == 0)
		for (uint32_t pe = 0; pe < o.nr_pes; ++pe) {
			uint32_t complete = 0, error = 0, mapped_pe = UINT32_MAX;
			uint32_t error_offset = UINT32_MAX;
			uint32_t actual = UINT32_MAX, expected = UINT32_MAX;
			int complete_rc = pimnic_mailbox_read_u32(
				set, pe, "collective_completed",
						    &complete) != 0 ||
			    pimnic_mailbox_read_u32(set, pe, "error_code", &error) != 0;
			if (complete_rc || complete != o.rounds || error != 0) {
				if (failures < 4)
					pimnic_mailbox_read_u32(set, pe, "pe_index",
								&mapped_pe);
				if (failures < 4)
					pimnic_mailbox_read_u32(set, pe,
								"first_error_offset",
								&error_offset);
				if (failures < 4)
					pimnic_mailbox_read_u32(
						set, pe, "collective_debug_actual", &actual);
				if (failures < 4)
					pimnic_mailbox_read_u32(
						set, pe, "collective_debug_expected", &expected);
				if (failures < 4)
					fprintf(stderr,
						"PE%u mapped=%u complete=%u error=%u offset=%u actual=%u expected=%u read_rc=%d\n",
						pe, mapped_pe, complete, error,
						error_offset, actual, expected, complete_rc);
				++failures;
			}
		}
	bool ok = rc == 0 && failures == 0 && result.status == PIMNIC_STATUS_OK &&
		  result.collision_faults == 0;
	printf("PIMNIC collective host %s prim=%u writeback=%u dim=%u "
	       "rounds=%u failed_pes=%u ci=%lu elapsed_s=%.3f\n",
	       ok ? "PASS" : "FAIL", o.prim, o.writeback, o.dim, o.rounds,
	       failures, result.ci_commands, result.elapsed_ns / 1e9);
	if (exporter) pimnic_export_close(exporter);
	if (set) pimnic_pe_set_free(set);
	if (ctx) pimnic_fini(ctx);
	return ok ? 0 : 1;
}
