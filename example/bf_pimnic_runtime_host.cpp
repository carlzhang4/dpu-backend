#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include <dpu.h>
#include <dpu_management.h>
#include <dpu_memory.h>
#include <dpu_program.h>
#include <dpu_rank.h>
#include <dpu_runner.h>
#include <ufi/ufi_ci.h>
#include <ufi_rank_utils.h>
}

#include "../bfdma/libr.hpp"
#include "../pimnic_bf3_runtime/control_protocol.h"
#include "../pimnic_bf3_runtime/mram_addr.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./build/example/bf_pimnic_runtime"
#endif

static bool stop_requested;

struct Options {
	uint32_t num_dpus = 64;
	uint32_t mode = PIMNIC_MODE_ECHO;
	uint32_t payload = PIMNIC_RUNTIME_DEFAULT_PAYLOAD;
	uint32_t messages = PIMNIC_RUNTIME_DEFAULT_MESSAGES;
	uint32_t batch = PIMNIC_RUNTIME_DEFAULT_BATCH;
	uint32_t poll_us = 10;
	uint32_t timeout_us = 500000;
	uint32_t pe_slowdown = 0;
	uint32_t nic_slowdown_us = 0;
	uint32_t active_group_mask = UINT32_MAX;
	int deactivate_group = -1;
	uint32_t reactivate_after_ms = 0;
	int port = 6666;
	int numa_node = 0;
	std::string device_name = "mlx5_0";
	std::string profile = "backend=hw";
};

struct RankInfo {
	struct dpu_rank_t *rank = nullptr;
	uint64_t base = 0;
};

struct Exporter {
	NetParam net;
	struct ibv_pd *pd = nullptr;
	std::vector<vhca_resource> resources;
	int control_fd = -1;
};

static void signal_handler(int)
{
	stop_requested = true;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [--mode rx-only|echo] [--num-dpus 64|128|...] "
		"[--payload N] [--messages N] [--batch N] [--poll-us N] "
		"[--timeout-us N] [--pe-slowdown N] [--port P] "
		"[--nic-slowdown-us N] [--active-mask MASK] "
		"[--deactivate-group G --reactivate-after-ms N] "
		"[--device-name DEV] [--numa-node N] [--profile PROFILE]\n",
		program);
}

static bool parse_u32(const char *value, uint32_t *output)
{
	char *end = nullptr;
	unsigned long parsed = strtoul(value, &end, 0);
	if (end == value || *end != '\0' || parsed > UINT32_MAX)
		return false;
	*output = (uint32_t)parsed;
	return true;
}

static bool parse_i32(const char *value, int *output)
{
	char *end = nullptr;
	long parsed = strtol(value, &end, 0);
	if (end == value || *end != '\0')
		return false;
	*output = (int)parsed;
	return true;
}

static bool parse_options(int argc, char **argv, Options *options)
{
	for (int index = 1; index < argc; ++index) {
		std::string argument = argv[index];
		if (argument == "--help" || argument == "-h") {
			usage(argv[0]);
			return false;
		}
		if (index + 1 == argc)
			return false;
		const char *value = argv[++index];
		if (argument == "--mode") {
			if (!strcmp(value, "rx-only"))
				options->mode = PIMNIC_MODE_RX_ONLY;
			else if (!strcmp(value, "echo"))
				options->mode = PIMNIC_MODE_ECHO;
			else
				return false;
		} else if (argument == "--num-dpus") {
			if (!parse_u32(value, &options->num_dpus))
				return false;
		} else if (argument == "--payload") {
			if (!parse_u32(value, &options->payload))
				return false;
		} else if (argument == "--messages") {
			if (!parse_u32(value, &options->messages))
				return false;
		} else if (argument == "--batch") {
			if (!parse_u32(value, &options->batch))
				return false;
		} else if (argument == "--poll-us") {
			if (!parse_u32(value, &options->poll_us))
				return false;
		} else if (argument == "--timeout-us") {
			if (!parse_u32(value, &options->timeout_us))
				return false;
		} else if (argument == "--pe-slowdown") {
			if (!parse_u32(value, &options->pe_slowdown))
				return false;
		} else if (argument == "--nic-slowdown-us") {
			if (!parse_u32(value, &options->nic_slowdown_us))
				return false;
		} else if (argument == "--active-mask") {
			if (!parse_u32(value, &options->active_group_mask))
				return false;
		} else if (argument == "--deactivate-group") {
			if (!parse_i32(value, &options->deactivate_group))
				return false;
		} else if (argument == "--reactivate-after-ms") {
			if (!parse_u32(value,
				       &options->reactivate_after_ms))
				return false;
		} else if (argument == "--port") {
			if (!parse_i32(value, &options->port))
				return false;
		} else if (argument == "--numa-node") {
			if (!parse_i32(value, &options->numa_node))
				return false;
		} else if (argument == "--device-name") {
			options->device_name = value;
		} else if (argument == "--profile") {
			options->profile = value;
		} else {
			fprintf(stderr, "unknown option: %s\n", argument.c_str());
			return false;
		}
	}
	return true;
}

static bool send_all(int fd, const void *buffer, size_t length)
{
	const uint8_t *cursor = (const uint8_t *)buffer;
	while (length != 0) {
		ssize_t sent = send(fd, cursor, length, 0);
		if (sent < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (sent == 0)
			return false;
		cursor += sent;
		length -= (size_t)sent;
	}
	return true;
}

static bool recv_all(int fd, void *buffer, size_t length)
{
	uint8_t *cursor = (uint8_t *)buffer;
	while (length != 0) {
		ssize_t received = recv(fd, cursor, length, MSG_WAITALL);
		if (received < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (received == 0)
			return false;
		cursor += received;
		length -= (size_t)received;
	}
	return true;
}

static uint64_t rank_base_address(struct dpu_rank_t *rank)
{
	dpu_rank_handler_t handler = rank->handler_context->handler;
	return handler->__get_base_region_address(rank);
}

static std::vector<RankInfo> collect_ranks(struct dpu_set_t set)
{
	std::vector<RankInfo> ranks;
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		struct dpu_rank_t *rank = dpu.dpu->rank;
		auto found = std::find_if(
			ranks.begin(), ranks.end(),
			[rank](const RankInfo &value) {
				return value.rank == rank;
			});
		if (found == ranks.end())
			ranks.push_back({ rank, rank_base_address(rank) });
	}
	std::sort(ranks.begin(), ranks.end(),
		  [](const RankInfo &left, const RankInfo &right) {
			  return left.base < right.base;
		  });
	return ranks;
}

static bool write_dpu_word(struct dpu_t *dpu, struct dpu_symbol_t symbol,
			   uint32_t value)
{
	dpuword_t word = value;
	return dpu_copy_to_wram_for_dpu(dpu, symbol.address >> 2, &word, 1) ==
	       DPU_OK;
}

static bool read_dpu_word(struct dpu_t *dpu, struct dpu_symbol_t symbol,
			  uint32_t *value)
{
	dpuword_t word = 0;
	dpu_error_t status = dpu_copy_from_wram_for_dpu(
		dpu, &word, symbol.address >> 2, 1);
	*value = word;
	return status == DPU_OK;
}

static bool initialize_dpus(struct dpu_set_t set,
			    struct dpu_program_t *program,
			    const Options &options)
{
	std::vector<uint8_t> zero_desc(PIMNIC_DESC_COUNT *
					       PIMNIC_DESC_ENTRY_BYTES,
				       0);
	uint64_t zero64 = 0;
	uint32_t zero32 = 0;
	DPU_ASSERT(dpu_copy_to(set, "rx_desc", 0, zero_desc.data(),
			       zero_desc.size()));
	DPU_ASSERT(dpu_copy_to(set, "tx_desc", 0, zero_desc.data(),
			       zero_desc.size()));
	DPU_ASSERT(dpu_copy_to(set, "pe_pub", 0, &zero64, sizeof(zero64)));
	DPU_ASSERT(dpu_copy_to(set, "nic_pub", 0, &zero64, sizeof(zero64)));

	struct dpu_symbol_t gate_command = {};
	struct dpu_symbol_t gate_ack = {};
	struct dpu_symbol_t stop = {};
	struct dpu_symbol_t lane_id = {};
	struct dpu_symbol_t runtime_mode = {};
	struct dpu_symbol_t slowdown = {};
	DPU_ASSERT(dpu_get_symbol(program, "gate_command", &gate_command));
	DPU_ASSERT(dpu_get_symbol(program, "gate_ack", &gate_ack));
	DPU_ASSERT(dpu_get_symbol(program, "stop", &stop));
	DPU_ASSERT(dpu_get_symbol(program, "lane_id", &lane_id));
	DPU_ASSERT(dpu_get_symbol(program, "runtime_mode", &runtime_mode));
	DPU_ASSERT(dpu_get_symbol(program, "slowdown_cycles", &slowdown));

	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		uint32_t lane =
			dpu.dpu->dpu_id < 4 ?
				dpu.dpu->slice_id :
				8u + dpu.dpu->slice_id;
		if (!write_dpu_word(dpu.dpu, gate_command, zero32) ||
		    !write_dpu_word(dpu.dpu, gate_ack, zero32) ||
		    !write_dpu_word(dpu.dpu, stop, zero32) ||
		    !write_dpu_word(dpu.dpu, lane_id, lane) ||
		    !write_dpu_word(dpu.dpu, runtime_mode, options.mode) ||
		    !write_dpu_word(dpu.dpu, slowdown,
				    options.pe_slowdown))
			return false;
	}
	return true;
}

static bool boot_without_polling(const std::vector<RankInfo> &ranks)
{
	for (const RankInfo &rank : ranks) {
		dpu_error_t status = dpu_boot_rank(rank.rank);
		if (status != DPU_OK) {
			fprintf(stderr, "dpu_boot_rank failed: %s\n",
				dpu_error_to_string(status));
			return false;
		}
	}
	printf("persistent kernels booted with dpu_boot_rank; "
	       "no async polling job registered\n");
	return true;
}

static void flush_ci_lines(uint64_t rank_base)
{
	uint8_t *command =
		(uint8_t *)(rank_base + PIMNIC_CI_COMMAND_OFFSET);
	uint8_t *response =
		(uint8_t *)(rank_base + PIMNIC_CI_RESPONSE_OFFSET);
	_mm_clflush(command);
	_mm_clflush(response);
	_mm_mfence();
}

static bool initialize_exporter(Exporter *exporter, const Options &options,
				const std::vector<RankInfo> &ranks)
{
	exporter->net.numNodes = 2;
	exporter->net.nodeId = 0;
	exporter->net.device_name = options.device_name;
	exporter->net.numa_node = options.numa_node;
	exporter->net.sock_port = options.port;
	exporter->net.sockfd = new int[128];
	exporter->net.ib_port = 1;
	exporter->net.page_size = sysconf(_SC_PAGESIZE);
	exporter->net.cacheline_size = get_cache_line_size();

	roce_init(exporter->net, 1);
	struct devx_hca_capabilities capabilities = {};
	if (devx_query_hca_caps(exporter->net.contexts[0], &capabilities) != 0)
		return false;
	exporter->pd = ibv_alloc_pd(exporter->net.contexts[0]);
	if (exporter->pd == nullptr)
		return false;

	const uint64_t export_bytes = 512ULL * 1024ULL * 1024ULL;
	exporter->resources.resize(ranks.size());
	uint8_t access_key[32];
	memset(access_key, 1, sizeof(access_key));
	for (size_t index = 0; index < ranks.size(); ++index) {
		vhca_resource &resource = exporter->resources[index];
		resource.pd = exporter->pd;
		resource.vhca_id = capabilities.vhca_id;
		resource.addr = (void *)ranks[index].base;
		resource.size = export_bytes;
		resource.mr = devx_reg_mr(
			exporter->pd, resource.addr, resource.size,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
		if (resource.mr == nullptr)
			return false;
		resource.mkey = devx_mr_query_mkey(resource.mr);
		if (devx_mr_allow_other_vhca_access(
			    resource.mr, access_key, sizeof(access_key)) != 0)
			return false;
		printf("export rank%zu base=%p size=%lu mkey=%u\n", index,
		       resource.addr, resource.size, resource.mkey);
	}

	socket_init(exporter->net);
	struct pimnic_resource_hello hello = {};
	hello.magic = PIMNIC_CTRL_MAGIC;
	hello.version = PIMNIC_CTRL_VERSION;
	hello.nr_ranks = (uint32_t)ranks.size();
	if (!send_all(exporter->net.sockfd[1], &hello, sizeof(hello)))
		return false;
	exchange_vhca_data(exporter->net, exporter->resources.data(),
			   exporter->resources.size());
	exporter->control_fd = exporter->net.sockfd[1];
	return exporter->control_fd >= 0;
}

static void destroy_exporter(Exporter *exporter)
{
	if (exporter->control_fd >= 0)
		close(exporter->control_fd);
	for (vhca_resource &resource : exporter->resources)
		if (resource.mr != nullptr)
			devx_dereg_mr(resource.mr);
	if (exporter->pd != nullptr)
		ibv_dealloc_pd(exporter->pd);
	delete[] exporter->net.sockfd;
}

static bool stop_ranks(struct dpu_set_t set,
		       struct dpu_program_t *program,
		       const std::vector<RankInfo> &ranks)
{
	struct dpu_symbol_t stop = {};
	dpu_error_t status = dpu_get_symbol(program, "stop", &stop);
	if (status != DPU_OK) {
		fprintf(stderr, "cannot resolve stop symbol: %s\n",
			dpu_error_to_string(status));
		return false;
	}
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		if (!write_dpu_word(dpu.dpu, stop, 1)) {
			fprintf(stderr,
				"low-level stop WRAM write failed ci=%u "
				"dpu=%u\n",
				dpu.dpu->slice_id, dpu.dpu->dpu_id);
			return false;
		}
	}

	for (const RankInfo &rank : ranks) {
		bool stopped = false;
		for (uint32_t retry = 0; retry < 100000; ++retry) {
			dpu_lock_rank(rank.rank);
			status = dpu_poll_rank(rank.rank);
			uint32_t running =
				dpu_get_run_context(rank.rank)->nb_dpu_running;
			dpu_unlock_rank(rank.rank);
			if (status != DPU_OK) {
				fprintf(stderr, "dpu_poll_rank failed: %s\n",
					dpu_error_to_string(status));
				return false;
			}
			if (running == 0) {
				stopped = true;
				break;
			}
			usleep(10);
		}
		if (!stopped) {
			fprintf(stderr, "rank at 0x%lx did not stop\n",
				rank.base);
			return false;
		}
	}
	return true;
}

static bool validate_dpus(struct dpu_set_t set,
			  struct dpu_program_t *program,
			  const Options &options,
			  const std::vector<RankInfo> &ranks,
			  uint32_t active_group_mask)
{
	struct dpu_symbol_t received = {};
	struct dpu_symbol_t echoed = {};
	struct dpu_symbol_t error = {};
	struct dpu_symbol_t first_error = {};
	struct dpu_symbol_t heartbeat = {};
	DPU_ASSERT(dpu_get_symbol(program, "messages_received", &received));
	DPU_ASSERT(dpu_get_symbol(program, "messages_echoed", &echoed));
	DPU_ASSERT(dpu_get_symbol(program, "error_code", &error));
	DPU_ASSERT(dpu_get_symbol(program, "first_error_offset",
				  &first_error));
	DPU_ASSERT(dpu_get_symbol(program, "runtime_heartbeat", &heartbeat));

	uint32_t checked = 0;
	uint32_t failed = 0;
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		auto rank_it = std::find_if(
			ranks.begin(), ranks.end(),
			[&dpu](const RankInfo &value) {
				return value.rank == dpu.dpu->rank;
			});
		if (rank_it == ranks.end())
			return false;
		uint32_t rank_index =
			(uint32_t)std::distance(ranks.begin(), rank_it);
		uint32_t group_in_rank =
			dpu.dpu->dpu_id < 4 ?
				dpu.dpu->dpu_id :
				dpu.dpu->dpu_id - 4;
		uint32_t global_group =
			rank_index * PIMNIC_GROUPS_PER_RANK +
			group_in_rank;
		bool active =
			(active_group_mask & (1u << global_group)) != 0;
		uint32_t expected_messages =
			active ? options.messages : 0;
		uint32_t got_received = 0;
		uint32_t got_echoed = 0;
		uint32_t got_error = 0;
		uint32_t got_first_error = 0;
		uint32_t got_heartbeat = 0;
		if (!read_dpu_word(dpu.dpu, received, &got_received) ||
		    !read_dpu_word(dpu.dpu, echoed, &got_echoed) ||
		    !read_dpu_word(dpu.dpu, error, &got_error) ||
		    !read_dpu_word(dpu.dpu, first_error, &got_first_error) ||
		    !read_dpu_word(dpu.dpu, heartbeat, &got_heartbeat))
			return false;
		bool ok =
			got_received == expected_messages &&
			(options.mode != PIMNIC_MODE_ECHO ||
			 got_echoed == expected_messages) &&
			got_error == 0 && got_heartbeat != 0;
		if (!ok) {
			fprintf(stderr,
				"DPU ci=%u dpu=%u received=%u echoed=%u "
				"error=%u first_error=%u heartbeat=%u\n",
				dpu.dpu->slice_id, dpu.dpu->dpu_id,
				got_received, got_echoed, got_error,
				got_first_error, got_heartbeat);
			++failed;
		}
		++checked;
	}
	printf("DPU validation checked=%u failed=%u\n", checked, failed);
	return checked != 0 && failed == 0;
}

int main(int argc, char **argv)
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	Options options;
	if (!parse_options(argc, argv, &options)) {
		usage(argv[0]);
		return 2;
	}
	if (options.num_dpus == 0 || options.num_dpus % 64u != 0 ||
	    options.num_dpus / 64u > PIMNIC_MAX_RANKS ||
	    options.payload < PIMNIC_RUNTIME_PAYLOAD_MIN ||
	    options.payload > PIMNIC_RUNTIME_PAYLOAD_MAX ||
	    options.messages == 0 || options.batch == 0 ||
	    options.batch >= PIMNIC_DESC_COUNT ||
	    options.deactivate_group >=
		    (int)(options.num_dpus /
			  PIMNIC_PE_GROUP_SIZE)) {
		fprintf(stderr, "invalid geometry or runtime parameters\n");
		return 2;
	}

	printf("PIM-centric control plane mode=%s dpus=%u payload=%u "
	       "messages/group=%u batch=%u poll_us=%u\n",
	       options.mode == PIMNIC_MODE_ECHO ? "echo" : "rx-only",
	       options.num_dpus, options.payload, options.messages,
	       options.batch, options.poll_us);

	struct dpu_set_t set;
	struct dpu_program_t *program = nullptr;
	DPU_ASSERT(dpu_alloc(options.num_dpus, options.profile.c_str(), &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY, &program));
	std::vector<RankInfo> ranks = collect_ranks(set);
	if (ranks.size() != options.num_dpus / 64u ||
	    !initialize_dpus(set, program, options)) {
		dpu_free(set);
		return 1;
	}

	struct dpu_symbol_t rx_desc = {};
	struct dpu_symbol_t tx_desc = {};
	struct dpu_symbol_t pe_pub = {};
	struct dpu_symbol_t nic_pub = {};
	struct dpu_symbol_t rx_data = {};
	struct dpu_symbol_t tx_data = {};
	struct dpu_symbol_t gate_command = {};
	struct dpu_symbol_t gate_ack = {};
	DPU_ASSERT(dpu_get_symbol(program, "rx_desc", &rx_desc));
	DPU_ASSERT(dpu_get_symbol(program, "tx_desc", &tx_desc));
	DPU_ASSERT(dpu_get_symbol(program, "pe_pub", &pe_pub));
	DPU_ASSERT(dpu_get_symbol(program, "nic_pub", &nic_pub));
	DPU_ASSERT(dpu_get_symbol(program, "rx_data", &rx_data));
	DPU_ASSERT(dpu_get_symbol(program, "tx_data", &tx_data));
	DPU_ASSERT(dpu_get_symbol(program, "gate_command", &gate_command));
	DPU_ASSERT(dpu_get_symbol(program, "gate_ack", &gate_ack));

	Exporter exporter = {};
	exporter.control_fd = -1;
	if (!initialize_exporter(&exporter, options, ranks)) {
		dpu_free(set);
		return 1;
	}
	if (!boot_without_polling(ranks)) {
		destroy_exporter(&exporter);
		dpu_free(set);
		return 1;
	}

	pimnic_runtime_config config = {};
	config.magic = PIMNIC_CTRL_MAGIC;
	config.version = PIMNIC_CTRL_VERSION;
	config.mode = options.mode;
	config.nr_ranks = (uint32_t)ranks.size();
	config.nr_groups = config.nr_ranks * PIMNIC_GROUPS_PER_RANK;
	config.payload_bytes = options.payload;
	config.messages_per_group = options.messages;
	config.batch_size = options.batch;
	config.poll_interval_us = options.poll_us;
	config.timeout_us = options.timeout_us;
	uint32_t valid_group_mask =
		config.nr_groups == 32 ? UINT32_MAX :
					((1u << config.nr_groups) - 1u);
	config.active_group_mask =
		options.active_group_mask & valid_group_mask;
	config.nic_slowdown_us = options.nic_slowdown_us;
	if (config.active_group_mask == 0) {
		fprintf(stderr, "active group mask selects no group\n");
		destroy_exporter(&exporter);
		dpu_free(set);
		return 2;
	}

	for (uint32_t index = 0; index < config.nr_ranks; ++index) {
		RankInfo &rank = ranks[index];
		uint64_t snapshot[8] = {};
		if (ci_get_color(rank.rank, nullptr) != DPU_OK ||
		    ci_update_commands(rank.rank, snapshot) != DPU_OK) {
			destroy_exporter(&exporter);
			dpu_free(set);
			return 1;
		}
		uint8_t nr_cis =
			rank.rank->description->hw.topology
				.nr_of_control_interfaces;
		pimnic_rank_config &rank_config = config.ranks[index];
		rank_config.rank_base = rank.base;
		rank_config.command_addr =
			rank.base + PIMNIC_CI_COMMAND_OFFSET;
		rank_config.response_addr =
			rank.base + PIMNIC_CI_RESPONSE_OFFSET;
		memcpy(rank_config.host_response, snapshot, sizeof(snapshot));
		rank_config.rx_desc_offset =
			pimnic_mram_logical_offset(rx_desc.address);
		rank_config.tx_desc_offset =
			pimnic_mram_logical_offset(tx_desc.address);
		rank_config.pe_pub_offset =
			pimnic_mram_logical_offset(pe_pub.address);
		rank_config.nic_pub_offset =
			pimnic_mram_logical_offset(nic_pub.address);
		rank_config.rx_data_offset =
			pimnic_mram_logical_offset(rx_data.address);
		rank_config.tx_data_offset =
			pimnic_mram_logical_offset(tx_data.address);
		rank_config.gate_command_word_addr =
			gate_command.address >> 2;
		rank_config.gate_ack_word_addr = gate_ack.address >> 2;
		rank_config.next_color = GET_CI_CONTEXT(rank.rank)->color;
		rank_config.nr_cis = nr_cis;
		rank_config.ci_mask =
			nr_cis >= 8 ? 0xffu : (uint8_t)((1u << nr_cis) - 1u);
		flush_ci_lines(rank.base);
	}

	printf("CI ownership handoff host->BF3 ranks=%u groups=%u; "
	       "host entering blocking result wait\n",
	       config.nr_ranks, config.nr_groups);
	fflush(stdout);
	uint64_t g_ci_data_ops = 0;
	bool ok = send_all(exporter.control_fd, &config, sizeof(config));
	if (ok && options.deactivate_group >= 0) {
		struct pimnic_group_control control = {};
		control.magic = PIMNIC_CTRL_MAGIC;
		control.version = PIMNIC_CTRL_VERSION;
		control.opcode = PIMNIC_GROUP_DEACTIVATE;
		control.group = (uint32_t)options.deactivate_group;
		ok = send_all(exporter.control_fd, &control,
			      sizeof(control));
		if (ok && options.reactivate_after_ms != 0) {
			usleep(options.reactivate_after_ms * 1000u);
			control.opcode = PIMNIC_GROUP_ACTIVATE;
			ok = send_all(exporter.control_fd, &control,
				      sizeof(control));
		}
	}
	pimnic_runtime_result result = {};
	if (ok)
		ok = recv_all(exporter.control_fd, &result, sizeof(result));
	if (!ok || result.magic != PIMNIC_CTRL_MAGIC ||
	    result.version != PIMNIC_CTRL_VERSION ||
	    result.status != PIMNIC_STATUS_OK) {
		fprintf(stderr,
			"BF3 result failed exchange=%d status=%u "
			"pattern=%lu order=%lu\n",
			ok, result.status, result.pattern_errors,
			result.order_errors);
		ok = false;
	}

	for (uint32_t index = 0; index < config.nr_ranks; ++index) {
		flush_ci_lines(ranks[index].base);
		if (ci_get_color(ranks[index].rank, nullptr) != DPU_OK)
			ok = false;
		printf("rank%u BF3_next_color=0x%02x host_next_color=0x%02x\n",
		       index, result.final_next_color[index],
		       GET_CI_CONTEXT(ranks[index].rank)->color);
	}

	bool stopped = stop_ranks(set, program, ranks);
	bool dpus_ok =
		stopped &&
		validate_dpus(set, program, options, ranks,
			      config.active_group_mask);
	ok = ok && dpus_ok && g_ci_data_ops == 0;
	printf("BF3 stats injected=%lu echoed=%lu pattern_errors=%lu "
	       "order_errors=%lu dma_reads=%lu dma_writes=%lu "
	       "read_bytes=%lu write_bytes=%lu ci_commands=%lu "
	       "collision=%lu controls=%lu rx_wraps=%lu tx_wraps=%lu "
	       "elapsed_s=%.3f rtt_p50_us=%.3f "
	       "rtt_p99_us=%.3f poll_p50_us=%.3f poll_p99_us=%.3f\n",
	       result.messages_injected, result.messages_echoed,
	       result.pattern_errors, result.order_errors, result.dma_reads,
	       result.dma_writes, result.dma_read_bytes,
	       result.dma_write_bytes, result.ci_commands,
	       result.collision_faults,
	       result.group_control_commands, result.rx_wraps,
	       result.tx_wraps, result.elapsed_ns / 1e9,
	       result.rtt_p50_ns / 1e3, result.rtt_p99_ns / 1e3,
	       result.poll_interval_p50_ns / 1e3,
	       result.poll_interval_p99_ns / 1e3);
	printf("g_ci_data_ops=%lu (steady-state host data-path CI ops)\n",
	       g_ci_data_ops);
	printf("PIM-centric-control-plane %s\n", ok ? "PASS" : "FAIL");

	destroy_exporter(&exporter);
	DPU_ASSERT(dpu_free(set));
	return ok && !stop_requested ? 0 : 1;
}
