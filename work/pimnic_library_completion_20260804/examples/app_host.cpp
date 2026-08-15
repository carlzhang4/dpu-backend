#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include "apps/common/app_harness.h"
#include "apps/kvstore/model.h"
#include "apps/select/model.h"
#include "pimnic/host/mailbox.h"
#include "pimnic/host/ops_upmem.h"

namespace {
struct Options {
	std::string app = "kvstore";
	std::string binary;
	std::string profile = "backend=hw";
	std::string device = "mlx5_0";
	uint32_t messages = 4;
	uint32_t batch = 4;
	uint32_t poll_us = 10;
	uint32_t timeout_us = 500000;
	uint32_t port = 7100;
	bool reference_only = false;
};

/* Wire layouts of the V2 app contracts; they byte-match the BF3 bench
 * and the original benchmarks (kvstore: 16-byte identity entries + 8-byte
 * value response; select: count header + pad-to-max odd-value body). */
struct RawKvEntry {
	char key[8];
	char value[8];
};
struct KvResponse {
	uint64_t value;
};
constexpr uint32_t kSelectRowsPerPe = 1000;
struct SelectResponse {
	uint32_t count;
	uint32_t values[kSelectRowsPerPe];
	uint32_t reserved;
};
static_assert(sizeof(SelectResponse) == 4008, "select response ABI size");

bool u32(const char *text, uint32_t *value)
{
	char *end = nullptr;
	unsigned long parsed = strtoul(text, &end, 0);
	if (end == text || *end != '\0' || parsed > UINT32_MAX)
		return false;
	*value = static_cast<uint32_t>(parsed);
	return true;
}

bool parse(int argc, char **argv, Options *o)
{
	for (int i = 1; i < argc; ++i) {
		std::string flag = argv[i];
		if (flag == "--reference-only") {
			o->reference_only = true;
			continue;
		}
		if (++i == argc)
			return false;
		const char *value = argv[i];
		if (flag == "--app") o->app = value;
		else if (flag == "--binary") o->binary = value;
		else if (flag == "--profile") o->profile = value;
		else if (flag == "--device-name") o->device = value;
		else if (flag == "--messages") { if (!u32(value, &o->messages)) return false; }
		else if (flag == "--batch") { if (!u32(value, &o->batch)) return false; }
		else if (flag == "--poll-us") { if (!u32(value, &o->poll_us)) return false; }
		else if (flag == "--timeout-us") { if (!u32(value, &o->timeout_us)) return false; }
		else if (flag == "--port") { if (!u32(value, &o->port)) return false; }
		else return false;
	}
	return o->app == "empty" || o->app == "kvstore" ||
	       o->app == "select" || o->app == "gnn";
}

uint64_t hash_bytes(const void *data, size_t bytes)
{
	const auto *cursor = static_cast<const uint8_t *>(data);
	uint64_t value = 1469598103934665603ULL;
	for (size_t i = 0; i < bytes; ++i) {
		value ^= cursor[i];
		value *= 1099511628211ULL;
	}
	return value;
}

uint64_t request_reference(const Options &o)
{
	uint64_t digest = 0;
	for (uint32_t message = 0; message < o.messages; ++message)
		for (uint32_t pe = 0; pe < 64; ++pe) {
			uint32_t request_id = message * 64u + pe;
			if (o.app == "kvstore") {
				KvResponse response{
					pimnic_kvstore_index(request_id)};
				digest ^= hash_bytes(&response, sizeof(response));
			} else {
				std::vector<uint32_t> rows(kSelectRowsPerPe);
				for (uint32_t i = 0; i < kSelectRowsPerPe; ++i)
					rows[i] = pe * kSelectRowsPerPe + i;
				auto matches = pimnic_select_run(rows);
				SelectResponse response{};
				response.count =
					static_cast<uint32_t>(matches.size());
				for (uint32_t i = 0; i < response.count; ++i)
					response.values[i] = matches[i];
				digest ^= hash_bytes(&response, sizeof(response));
			}
		}
	return digest;
}

struct GnnReference {
	uint64_t aggregate = 0;
	std::array<uint32_t, 64> per_pe{};
};

GnnReference gnn_reference(uint32_t cycles)
{
	using Feature = std::array<int32_t, 4>;
	std::array<Feature, 64> feature{};
	std::array<uint32_t, 64> digest{};
	for (uint32_t pe = 0; pe < 64; ++pe) {
		digest[pe] = 2166136261u;
		for (uint32_t i = 0; i < 4; ++i)
			feature[pe][i] = static_cast<int32_t>(pe * 4u + i);
	}
	for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
		auto contribution = feature;
		for (auto &entry : contribution)
			for (int32_t &value : entry)
				value += static_cast<int32_t>(cycle);
		if ((cycle & 1u) == 0) {
			for (uint32_t row = 0; row < 8; ++row)
				for (uint32_t col = 0; col < 8; ++col)
					feature[row * 8u + col] = contribution[row * 8u];
		} else {
			for (uint32_t col = 0; col < 8; ++col)
				for (uint32_t row = 0; row < 8; ++row)
					feature[row * 8u + col] = contribution[col];
		}
		for (uint32_t pe = 0; pe < 64; ++pe) {
			const uint8_t *bytes = reinterpret_cast<const uint8_t *>(
				feature[pe].data());
			for (size_t i = 0; i < sizeof(Feature); ++i) {
				digest[pe] ^= bytes[i];
				digest[pe] *= 16777619u;
			}
		}
	}
	GnnReference result;
	result.per_pe = digest;
	result.aggregate = hash_bytes(digest.data(), sizeof(digest));
	return result;
}

void append_bytes(std::vector<uint8_t> *out, const void *data, size_t bytes)
{
	const auto *first = static_cast<const uint8_t *>(data);
	out->insert(out->end(), first, first + bytes);
}
} // namespace

int main(int argc, char **argv)
{
	Options o;
	if (!parse(argc, argv, &o) || o.batch == 0 ||
	    o.batch >= PIMNIC_DESC_COUNT || o.port > 65535) {
		fprintf(stderr, "invalid app_host arguments\n");
		return 2;
	}
	auto reference_start = std::chrono::steady_clock::now();
	uint64_t reference = o.app == "gnn" ? gnn_reference(o.messages).aggregate :
		(o.app == "empty" ? 0 : request_reference(o));
	uint64_t reference_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - reference_start).count();
	if (o.reference_only) {
		printf("PIMNIC APP reference app=%s digest=%016lx elapsed_ns=%lu\n",
		       o.app.c_str(), reference, reference_ns);
		return 0;
	}
	if (o.binary.empty())
		o.binary = o.app == "empty" ? "./build/paradigm_echo_pe" :
			     "./build/" + o.app + "_pe";

	PimnicAppPlan plan;
	plan.nr_pes = 64;
	plan.topology.nr_dims = o.app == "gnn" ? 2 : 1;
	plan.topology.dims[0] = o.app == "gnn" ? 8 : 64;
	plan.topology.dims[1] = o.app == "gnn" ? 8 : 0;
	plan.dpu_binary = o.binary;
	if (o.app == "kvstore") {
		PimnicAppPreload preload;
		preload.symbol = "key_entry_array";
		preload.bytes_per_pe = static_cast<uint32_t>(
			PIMNIC_KV_TOTAL_ENTRIES * sizeof(RawKvEntry));
		preload.dims = {{0, PIMNIC_PRIM_BROADCAST, 0, 0}};
		preload.data.resize(preload.bytes_per_pe);
		for (uint64_t i = 0; i < PIMNIC_KV_TOTAL_ENTRIES; ++i) {
			RawKvEntry *entry = reinterpret_cast<RawKvEntry *>(
				preload.data.data()) + i;
			memcpy(entry->key, &i, sizeof(i));
			memcpy(entry->value, &i, sizeof(i));
		}
		plan.preloads.push_back(std::move(preload));
	} else if (o.app == "select") {
		PimnicAppPreload preload;
		preload.symbol = "select_rows";
		preload.bytes_per_pe = kSelectRowsPerPe * sizeof(uint32_t);
		preload.dims = {{0, PIMNIC_PRIM_SCATTER, 0, 0}};
		for (uint32_t value = 0; value < 64u * kSelectRowsPerPe;
		     ++value)
			append_bytes(&preload.data, &value, sizeof(value));
		plan.preloads.push_back(std::move(preload));
	} else if (o.app == "gnn") {
		PimnicAppPreload preload;
		preload.symbol = "gnn_vector";
		preload.bytes_per_pe = 4u * sizeof(int32_t);
		preload.dims = {{0, PIMNIC_PRIM_SCATTER, 0, 0},
				{1, PIMNIC_PRIM_SCATTER, 0, 0}};
		for (int32_t value = 0; value < 64 * 4; ++value)
			append_bytes(&preload.data, &value, sizeof(value));
		plan.preloads.push_back(std::move(preload));
		for (uint32_t dim = 0; dim < 2; ++dim) {
			pimnic_collective_spec_t spec{};
			spec.dim = dim;
			spec.prim = dim == 0 ? PIMNIC_PRIM_REDUCE :
						 PIMNIC_PRIM_GATHER;
			spec.root = 0;
			spec.writeback = 1;
			spec.bytes_per_pe = 16;
			spec.tag = 0x40u + dim;
			plan.collectives.push_back(spec);
		}
	}

	pimnic_upmem_params_t upmem{o.device.c_str(), 0,
				     static_cast<int>(o.port)};
	pimnic_init_params_t init{pimnic_upmem_host_ops(), &upmem,
				  o.profile.c_str()};
	pimnic_ctx_t *ctx = nullptr;
	int rc = pimnic_init(&init, &ctx);
	PimnicAppHarness harness;
	if (rc == 0) rc = harness.deploy(ctx, plan);
	std::vector<uint32_t> values(64, 1);
	if (rc == 0 && o.app == "kvstore")
		rc = pimnic_pe_set_config_u32(harness.set(), "kv_request_tag",
					      values.data());
	if (rc == 0 && o.app == "select") {
		rc = pimnic_pe_set_config_u32(harness.set(), "select_query_tag",
					      values.data());
		values.assign(64, kSelectRowsPerPe);
		if (rc == 0)
			rc = pimnic_pe_set_config_u32(harness.set(), "select_row_count",
						      values.data());
	}
	if (rc == 0 && o.app == "gnn") {
		values.assign(64, harness.collective_ids()[0]);
		rc = pimnic_pe_set_config_u32(harness.set(), "gnn_collective_id",
					      values.data());
		values.assign(64, harness.collective_ids()[1]);
		if (rc == 0) rc = pimnic_pe_set_config_u32(
			harness.set(), "gnn_collective_id_alt", values.data());
		values.assign(64, o.messages);
		if (rc == 0) rc = pimnic_pe_set_config_u32(
			harness.set(), "gnn_cycles", values.data());
	}
	if (rc == 0) rc = harness.boot();

	pimnic_session_params_t session{};
	session.poll_interval_us = o.poll_us;
	session.timeout_us = o.timeout_us;
	session.active_group_mask = 0xf;
	pimnic_app_runtime_config app{};
	app.magic = PIMNIC_APP_MAGIC;
	app.kind = o.app == "empty" ? PIMNIC_APP_ECHO :
		(o.app == "kvstore" ? PIMNIC_APP_KVSTORE :
		 (o.app == "select" ? PIMNIC_APP_SELECT : PIMNIC_APP_GNN));
	app.mode = o.app == "empty" ? PIMNIC_APP_MODE_RX_ONLY :
					 PIMNIC_APP_MODE_ECHO;
	/* Largest message (header included) in either direction: select
	 * answers with the 4008-byte pad-to-max result. */
	app.payload_bytes = o.app == "select" ?
		16u + static_cast<uint32_t>(sizeof(SelectResponse)) : 24u;
	app.messages_per_group = o.app == "empty" ? 0 : o.messages;
	app.batch_size = o.batch;
	app.topology_dims = o.app == "gnn" ? (8u | (8u << 16)) : 64u;
	if (o.app == "gnn") {
		app.nr_collectives = 2;
		for (uint32_t i = 0; i < 2; ++i) {
			const auto &spec = plan.collectives[i];
			app.collectives[i].id = harness.collective_ids()[i];
			app.collectives[i].meta = spec.dim |
				(static_cast<uint32_t>(spec.prim) << 8) |
				(spec.root << 16) | (1u << 24);
			app.collectives[i].bytes_per_pe = spec.bytes_per_pe;
			app.collectives[i].tag = spec.tag;
		}
	}
	memcpy(session.app_config, &app, sizeof(app));
	pimnic_export_params export_params{nullptr,
					   static_cast<uint16_t>(o.port), 0};
	pimnic_runtime_result result{};
	timespec cpu_before{}, cpu_after{};
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_before);
	auto wall_before = std::chrono::steady_clock::now();
	if (rc == 0) rc = harness.run(export_params, session, &result);
	auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - wall_before).count();
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_after);
	uint64_t cpu_ns = static_cast<uint64_t>(
		static_cast<int64_t>(cpu_after.tv_sec - cpu_before.tv_sec) *
			1000000000LL + cpu_after.tv_nsec - cpu_before.tv_nsec);
	double cpu_percent = wall_ns == 0 ? 0.0 : 100.0 * cpu_ns / wall_ns;
	if (harness.set() != nullptr && harness.stop(5000000) != 0 && rc == 0)
		rc = -1;

	uint64_t observed = result.app_digest;
	uint32_t failed_pes = 0;
	if (rc == 0 && o.app == "gnn") {
		GnnReference golden = gnn_reference(o.messages);
		std::array<uint32_t, 64> observed_per_pe{};
		for (uint32_t pe = 0; pe < 64; ++pe) {
			uint32_t completed = 0, digest = 0;
			if (pimnic_mailbox_read_u32(harness.set(), pe, "gnn_completed",
						    &completed) != 0 ||
			    pimnic_mailbox_read_u32(harness.set(), pe, "gnn_digest",
						    &digest) != 0 ||
			    completed != o.messages || digest != golden.per_pe[pe])
				++failed_pes;
			if (failed_pes <= 4 &&
			    (completed != o.messages || digest != golden.per_pe[pe]))
				fprintf(stderr,
					"GNN PE%u completed=%u/%u digest=%08x expected=%08x\n",
					pe, completed, o.messages, digest,
					golden.per_pe[pe]);
			observed_per_pe[pe] = digest;
		}
		observed = hash_bytes(observed_per_pe.data(), sizeof(observed_per_pe));
	}
	bool equal = observed == reference;
	bool ok = rc == 0 && result.status == PIMNIC_STATUS_OK &&
		  result.data_errors == 0 && result.collision_faults == 0 &&
		  failed_pes == 0 && equal;
	printf("PIMNIC APP %s app=%s rc=%d status=%u v2_equal=%u digest=%016lx reference=%016lx "
	       "failed_pes=%u injected=%lu completed=%lu ci=%lu elapsed_ns=%lu "
	       "host_data_cpu_pct=%.3f reference_ns=%lu\n",
	       ok ? "PASS" : "FAIL", o.app.c_str(), rc, result.status,
	       equal ? 1u : 0u,
	       observed, reference, failed_pes, result.messages_injected,
	       result.messages_echoed, result.ci_commands, result.elapsed_ns,
	       cpu_percent, reference_ns);
	harness.reset();
	if (ctx != nullptr) pimnic_fini(ctx);
	return ok ? 0 : 1;
}
