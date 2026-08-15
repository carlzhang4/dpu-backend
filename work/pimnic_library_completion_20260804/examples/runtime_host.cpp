#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "pimnic/host/handoff.h"
#include "pimnic/host/mailbox.h"
#include "pimnic/host/ops_upmem.h"

struct Options {
	uint32_t nr_pes = 64;
	uint32_t payload = 1024;
	uint32_t messages = 64;
	uint32_t batch = 16;
	uint32_t poll_us = 10;
	uint32_t timeout_us = 500000;
	uint32_t pe_slowdown = 0;
	uint32_t reactivate_ms = 0;
	int deactivate_group = -1;
	int port = 6666;
	std::string mode = "echo";
	std::string profile = "backend=hw";
	std::string device = "mlx5_0";
	std::string binary = "./build/paradigm_echo_pe";
};

static bool parse_u32(const char *text, uint32_t *value)
{
	char *end = nullptr;
	unsigned long parsed = strtoul(text, &end, 0);
	if (end == text || *end != '\0' || parsed > UINT32_MAX)
		return false;
	*value = static_cast<uint32_t>(parsed);
	return true;
}

static bool parse(int argc, char **argv, Options *options)
{
	for (int i = 1; i < argc; ++i) {
		if (i + 1 == argc)
			return false;
		std::string flag = argv[i++];
		const char *value = argv[i];
		if (flag == "--num-dpus") {
			if (!parse_u32(value, &options->nr_pes)) return false;
		} else if (flag == "--payload") {
			if (!parse_u32(value, &options->payload)) return false;
		} else if (flag == "--messages") {
			if (!parse_u32(value, &options->messages)) return false;
		} else if (flag == "--batch") {
			if (!parse_u32(value, &options->batch)) return false;
		} else if (flag == "--poll-us") {
			if (!parse_u32(value, &options->poll_us)) return false;
		} else if (flag == "--timeout-us") {
			if (!parse_u32(value, &options->timeout_us)) return false;
		} else if (flag == "--port") {
			options->port = atoi(value);
		} else if (flag == "--profile") {
			options->profile = value;
		} else if (flag == "--device-name") {
			options->device = value;
		} else if (flag == "--binary") {
			options->binary = value;
		} else if (flag == "--pe-slowdown") {
			if (!parse_u32(value, &options->pe_slowdown)) return false;
		} else if (flag == "--reactivate-ms") {
			if (!parse_u32(value, &options->reactivate_ms)) return false;
		} else if (flag == "--deactivate-group") {
			options->deactivate_group = atoi(value);
		} else if (flag == "--mode") {
			options->mode = value;
		} else {
			return false;
		}
	}
	return true;
}

int main(int argc, char **argv)
{
	Options options;
	if (!parse(argc, argv, &options) || options.nr_pes == 0 ||
	    options.nr_pes % 64 != 0 || options.payload < PIMNIC_PAYLOAD_MIN ||
	    options.payload > PIMNIC_PAYLOAD_MAX || options.messages == 0 ||
	    options.batch == 0 || options.batch >= PIMNIC_DESC_COUNT ||
	    (options.mode != "echo" && options.mode != "rx-only")) {
		fprintf(stderr, "invalid runtime_host arguments\n");
		return 2;
	}
	pimnic_upmem_params_t upmem{ options.device.c_str(), 0, options.port };
	pimnic_init_params_t init{ pimnic_upmem_host_ops(), &upmem,
				   options.profile.c_str() };
	pimnic_ctx_t *ctx = nullptr;
	pimnic_pe_set_t *set = nullptr;
	pimnic_exporter_t *exporter = nullptr;
	int rc = pimnic_init(&init, &ctx);
	pimnic_topology_t topology{};
	topology.nr_dims = 1;
	topology.dims[0] = options.nr_pes;
	if (rc == 0)
		rc = pimnic_alloc_PE(ctx, options.nr_pes, &topology, &set);
	if (rc == 0)
		rc = pimnic_pe_set_load(set, options.binary.c_str());
	std::vector<uint32_t> per_pe(options.nr_pes,
		options.mode == "echo" ? PIMNIC_APP_MODE_ECHO :
					 PIMNIC_APP_MODE_RX_ONLY);
	if (rc == 0)
		rc = pimnic_pe_set_config_u32(set, "runtime_mode", per_pe.data());
	per_pe.assign(options.nr_pes, options.pe_slowdown);
	if (rc == 0)
		rc = pimnic_pe_set_config_u32(set, "slowdown_cycles", per_pe.data());

	pimnic_export_params export_params{ nullptr,
					 static_cast<uint16_t>(options.port), 0 };
	if (rc == 0)
		rc = pimnic_export_open(set, &export_params, &exporter);
	if (rc == 0)
		rc = pimnic_pe_set_boot(set);

	pimnic_session_params_t session{};
	session.poll_interval_us = options.poll_us;
	session.timeout_us = options.timeout_us;
	uint32_t groups = options.nr_pes / PIMNIC_PE_GROUP_SIZE;
	session.active_group_mask = groups == 32 ? UINT32_MAX :
						      (1u << groups) - 1u;
	pimnic_app_runtime_config app{};
	app.magic = PIMNIC_APP_MAGIC;
	app.kind = PIMNIC_APP_ECHO;
	app.mode = options.mode == "echo" ? PIMNIC_APP_MODE_ECHO :
					   PIMNIC_APP_MODE_RX_ONLY;
	app.payload_bytes = options.payload;
	app.messages_per_group = options.messages;
	app.batch_size = options.batch;
	app.topology_dims = options.nr_pes;
	memcpy(session.app_config, &app, sizeof(app));
	pimnic_runtime_config config{};
	if (rc == 0)
		rc = pimnic_handoff_build_config(set, exporter, &session, &config);
	if (rc == 0)
		rc = pimnic_handoff_start(exporter, &config);
	if (rc == 0 && options.deactivate_group >= 0) {
		rc = pimnic_group_set_active(
			exporter, static_cast<uint32_t>(options.deactivate_group), 0);
		if (rc == 0 && options.reactivate_ms != 0) {
			usleep(options.reactivate_ms * 1000u);
			rc = pimnic_group_set_active(
				exporter,
				static_cast<uint32_t>(options.deactivate_group), 1);
		}
	}
	pimnic_runtime_result result{};
	if (rc == 0)
		rc = pimnic_handoff_wait_result(exporter, &result, -1);
	if (rc == 0)
		rc = pimnic_handoff_reclaim(set);
	if (set != nullptr)
		if (pimnic_pe_set_stop(set, 5000000) != 0 && rc == 0)
			rc = -1;

	uint32_t failed = 0;
	if (rc == 0)
		for (uint32_t pe = 0; pe < options.nr_pes; ++pe) {
			uint32_t received = 0, echoed = 0;
			if (pimnic_mailbox_read_u32(set, pe, "messages_received",
						    &received) != 0 ||
			    pimnic_mailbox_read_u32(set, pe, "messages_echoed",
						    &echoed) != 0 ||
			    received != options.messages ||
			    echoed != (options.mode == "echo" ? options.messages : 0))
				++failed;
		}
	bool ok = rc == 0 && failed == 0 && result.status == PIMNIC_STATUS_OK &&
		  result.data_errors == 0 && result.order_errors == 0 &&
		  result.collision_faults == 0 && result.elapsed_ns != 0;
	printf("PIMNIC runtime host %s status=%u failed_pes=%u injected=%lu "
	       "echoed=%lu ci=%lu dma_r=%lu dma_w=%lu elapsed_s=%.3f\n",
	       ok ? "PASS" : "FAIL", result.status, failed,
	       result.messages_injected, result.messages_echoed,
	       result.ci_commands, result.dma_reads, result.dma_writes,
	       result.elapsed_ns / 1e9);
	if (exporter != nullptr)
		pimnic_export_close(exporter);
	if (set != nullptr)
		pimnic_pe_set_free(set);
	if (ctx != nullptr)
		pimnic_fini(ctx);
	return ok ? 0 : 1;
}
