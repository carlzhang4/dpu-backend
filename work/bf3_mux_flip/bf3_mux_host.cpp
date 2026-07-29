#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <x86intrin.h>

#include <string>

extern "C" {
#include <dpu.h>
#include <dpu_management.h>
#include <dpu_memory.h>
#include <dpu_program.h>
#include <dpu_rank.h>
#include <dpu_runner.h>
#include <ufi/ufi_ci.h>
#include <ufi/ufi_config.h>
#include <ufi_rank_utils.h>
}

#include "../../bfdma/libr.hpp"
#include "bf3_mux_protocol.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./build/example/bf3_mux_runtime"
#endif

static bool stop_requested;

struct Options {
	uint32_t mode = 0;
	uint32_t iterations = 0;
	uint32_t timeout_us = 500000;
	uint32_t pair_index = 0;
	uint32_t num_dpus = 64;
	int port = 6677;
	int numa_node = 0;
	std::string device_name = "mlx5_0";
	std::string profile = "backend=hw";
};

static uint32_t mux_mram_logical_offset(uint32_t symbol_address)
{
	return symbol_address & ~0x08000000u;
}

static uint32_t mux_mram_translate_offset(uint32_t logical_offset)
{
	const uint32_t mask_21_to_15 =
		((1u << (21 - 15 + 1)) - 1u) << 15;
	const uint32_t mask_21_to_14 =
		((1u << (21 - 14 + 1)) - 1u) << 14;
	uint32_t bits_21_to_15 =
		(logical_offset & mask_21_to_15) >> 15;
	uint32_t bit_14 = (logical_offset >> 14) & 1u;
	uint32_t unchanged_bits = logical_offset & ~mask_21_to_14;

	return unchanged_bits | (bits_21_to_15 << 14) | (bit_14 << 21);
}

static uint64_t mux_group_base_offset(uint32_t logical_offset)
{
	const uint64_t pe_lane_bytes = 8;
	const uint64_t pe_group_unit_bytes = 128;
	const uint64_t bank_chunk_bytes = 0x20000;
	const uint64_t bank_next_chunk_bytes = 0x100000;
	uint32_t translated = mux_mram_translate_offset(logical_offset);
	uint64_t true_byte =
		(translated / pe_lane_bytes) * pe_group_unit_bytes;

	return (true_byte % bank_chunk_bytes) +
	       (true_byte / bank_chunk_bytes) * bank_next_chunk_bytes;
}

struct Exporter {
	NetParam net_param;
	vhca_resource resource;
	int control_fd = -1;
};

static void signal_handler(int)
{
	stop_requested = true;
}

static bool send_all(int fd, const void *buffer, size_t length)
{
	const uint8_t *cursor = static_cast<const uint8_t *>(buffer);
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
		length -= static_cast<size_t>(sent);
	}
	return true;
}

static bool recv_all(int fd, void *buffer, size_t length)
{
	uint8_t *cursor = static_cast<uint8_t *>(buffer);
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
		length -= static_cast<size_t>(received);
	}
	return true;
}

static uint32_t parse_mode(const char *value)
{
	if (!strcmp(value, "x1") || !strcmp(value, "read"))
		return BF3_MUX_X1_READ_RESPONSE;
	if (!strcmp(value, "x2") || !strcmp(value, "identity"))
		return BF3_MUX_X2_IDENTITY;
	if (!strcmp(value, "x3") || !strcmp(value, "flip"))
		return BF3_MUX_X3_FLIP;
	if (!strcmp(value, "x4") || !strcmp(value, "runtime"))
		return BF3_MUX_X4_RUNTIME;
	if (!strcmp(value, "x5") || !strcmp(value, "bench"))
		return BF3_MUX_X5_BENCH;
	if (!strcmp(value, "x6") || !strcmp(value, "stress"))
		return BF3_MUX_X6_STRESS;
	return 0;
}

static const char *mode_name(uint32_t mode)
{
	switch (mode) {
	case BF3_MUX_X1_READ_RESPONSE:
		return "X1-read-response";
	case BF3_MUX_X2_IDENTITY:
		return "X2-identity";
	case BF3_MUX_X3_FLIP:
		return "X3-full-mux";
	case BF3_MUX_X4_RUNTIME:
		return "X4-runtime-window";
	case BF3_MUX_X5_BENCH:
		return "X5-latency";
	case BF3_MUX_X6_STRESS:
		return "X6-long-stress";
	default:
		return "unknown";
	}
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --mode x1|x2|x3|x4|x5|x6 "
		"[--iterations N] [--timeout-us N] [--pair N] "
		"[--port P] [--device-name DEV] [--numa-node N] "
		"[--profile PROFILE]\n",
		program);
}

static bool parse_u32(const char *value, uint32_t *output)
{
	char *end = nullptr;
	unsigned long parsed = strtoul(value, &end, 0);
	if (end == value || *end != '\0' || parsed > UINT32_MAX)
		return false;
	*output = static_cast<uint32_t>(parsed);
	return true;
}

static bool parse_i32(const char *value, int *output)
{
	char *end = nullptr;
	long parsed = strtol(value, &end, 0);
	if (end == value || *end != '\0')
		return false;
	*output = static_cast<int>(parsed);
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
			options->mode = parse_mode(value);
		} else if (argument == "--iterations") {
			if (!parse_u32(value, &options->iterations))
				return false;
		} else if (argument == "--timeout-us") {
			if (!parse_u32(value, &options->timeout_us))
				return false;
		} else if (argument == "--pair") {
			if (!parse_u32(value, &options->pair_index))
				return false;
		} else if (argument == "--num-dpus") {
			if (!parse_u32(value, &options->num_dpus))
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

	if (options->mode == 0 || options->pair_index > 3 ||
	    options->num_dpus == 0)
		return false;
	if (options->iterations == 0) {
		options->iterations =
			options->mode == BF3_MUX_X5_BENCH ? 100000 :
			options->mode == BF3_MUX_X6_STRESS ? 10000000 :
							     1;
	}
	return true;
}

static uint64_t rank_base_address(struct dpu_rank_t *rank)
{
	dpu_rank_handler_t handler = rank->handler_context->handler;
	return handler->__get_base_region_address(rank);
}

static struct dpu_rank_t *first_rank(struct dpu_set_t set)
{
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		return dpu.dpu->rank;
	}
	return nullptr;
}

static void flush_ci_lines(uint64_t rank_base)
{
	uint8_t *command =
		reinterpret_cast<uint8_t *>(rank_base + BF3_MUX_COMMAND_OFFSET);
	uint8_t *response =
		reinterpret_cast<uint8_t *>(rank_base + BF3_MUX_RESPONSE_OFFSET);
	_mm_clflush(command);
	_mm_clflush(response);
	_mm_mfence();
}

static bool init_exporter(Exporter *exporter, const Options &options,
			  void *address, uint64_t size)
{
	exporter->net_param.numNodes = 2;
	exporter->net_param.nodeId = 0;
	exporter->net_param.device_name = options.device_name;
	exporter->net_param.numa_node = options.numa_node;
	exporter->net_param.sock_port = options.port;
	exporter->net_param.sockfd = new int[128];
	exporter->net_param.ib_port = 1;
	exporter->net_param.page_size = sysconf(_SC_PAGESIZE);
	exporter->net_param.cacheline_size = get_cache_line_size();

	roce_init(exporter->net_param, 1);

	struct devx_hca_capabilities capabilities = {};
	if (devx_query_hca_caps(exporter->net_param.contexts[0],
				&capabilities) != 0) {
		fprintf(stderr, "devx_query_hca_caps failed\n");
		return false;
	}

	exporter->resource.pd = ibv_alloc_pd(exporter->net_param.contexts[0]);
	if (exporter->resource.pd == nullptr) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		return false;
	}

	exporter->resource.vhca_id = capabilities.vhca_id;
	exporter->resource.addr = address;
	exporter->resource.size = size;
	exporter->resource.mr =
		devx_reg_mr(exporter->resource.pd, address, size,
			    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				    IBV_ACCESS_REMOTE_WRITE |
				    IBV_ACCESS_RELAXED_ORDERING);
	if (exporter->resource.mr == nullptr) {
		fprintf(stderr, "devx_reg_mr failed\n");
		return false;
	}
	exporter->resource.mkey = devx_mr_query_mkey(exporter->resource.mr);

	uint8_t access_key[32];
	memset(access_key, 1, sizeof(access_key));
	if (devx_mr_allow_other_vhca_access(exporter->resource.mr, access_key,
					    sizeof(access_key)) != 0) {
		fprintf(stderr, "devx_mr_allow_other_vhca_access failed\n");
		return false;
	}

	printf("PIM1 export addr=%p size=%lu vhca=%u mkey=%u port=%d\n",
	       address, size, capabilities.vhca_id, exporter->resource.mkey,
	       options.port);
	fflush(stdout);

	socket_init(exporter->net_param);
	exchange_vhca_data(exporter->net_param, &exporter->resource, 1);
	exporter->control_fd = exporter->net_param.sockfd[1];
	return exporter->control_fd >= 0;
}

static void destroy_exporter(Exporter *exporter)
{
	if (exporter->control_fd >= 0)
		close(exporter->control_fd);
	if (exporter->resource.mr != nullptr)
		devx_dereg_mr(exporter->resource.mr);
	if (exporter->resource.pd != nullptr)
		ibv_dealloc_pd(exporter->resource.pd);
	delete[] exporter->net_param.sockfd;
}

static bool boot_without_polling(struct dpu_rank_t *rank)
{
	dpu_error_t status = dpu_boot_rank(rank);
	if (status != DPU_OK) {
		fprintf(stderr, "dpu_boot_rank failed: %s\n",
			dpu_error_to_string(status));
		return false;
	}
	printf("DPU kernel booted through low-level dpu_boot_rank; "
	       "no async polling job was registered\n");
	fflush(stdout);
	return true;
}

static bool stop_runtime(struct dpu_set_t set, struct dpu_rank_t *rank)
{
	uint32_t one = 1;
	dpu_error_t status = dpu_copy_to(set, "stop", 0, &one, sizeof(one));
	if (status != DPU_OK) {
		fprintf(stderr, "failed to write stop: %s\n",
			dpu_error_to_string(status));
		return false;
	}

	for (uint32_t retry = 0; retry < 100000; ++retry) {
		dpu_lock_rank(rank);
		status = dpu_poll_rank(rank);
		uint32_t running = dpu_get_run_context(rank)->nb_dpu_running;
		dpu_unlock_rank(rank);
		if (status != DPU_OK) {
			fprintf(stderr, "dpu_poll_rank failed: %s\n",
				dpu_error_to_string(status));
			return false;
		}
		if (running == 0)
			return true;
		usleep(10);
	}

	fprintf(stderr, "timeout waiting for runtime kernel to stop\n");
	return false;
}

static bool read_dpu_u32(struct dpu_t *dpu, struct dpu_symbol_t symbol,
			 uint32_t *value)
{
	dpuword_t word = 0;
	dpu_error_t status = dpu_copy_from_wram_for_dpu(
		dpu, &word, symbol.address >> 2, 1);
	*value = word;
	return status == DPU_OK;
}

static bool read_dpu_u64(struct dpu_t *dpu, struct dpu_symbol_t symbol,
			 uint64_t *value)
{
	dpuword_t words[2] = {};
	dpu_error_t status = dpu_copy_from_wram_for_dpu(
		dpu, words, symbol.address >> 2, 2);
	memcpy(value, words, sizeof(*value));
	return status == DPU_OK;
}

static bool validate_runtime(struct dpu_set_t set, struct dpu_program_t *program,
			     uint32_t mode, uint64_t pattern)
{
	struct dpu_symbol_t pass_symbol = {};
	struct dpu_symbol_t heartbeat_symbol = {};
	struct dpu_symbol_t observed_symbol = {};
	DPU_ASSERT(dpu_get_symbol(program, "pass", &pass_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "heartbeat", &heartbeat_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "observed_pattern",
				  &observed_symbol));

	uint32_t checked = 0;
	uint32_t passed = 0;
	uint32_t heartbeats = 0;
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		uint32_t heartbeat = 0;
		if (!read_dpu_u32(dpu.dpu, heartbeat_symbol, &heartbeat))
			return false;
		if (heartbeat != 0)
			++heartbeats;

		if (mode == BF3_MUX_X4_RUNTIME &&
		    (dpu.dpu->dpu_id == 0 || dpu.dpu->dpu_id == 4)) {
			uint32_t pass = 0;
			uint64_t observed = 0;
			++checked;
			if (!read_dpu_u32(dpu.dpu, pass_symbol, &pass) ||
			    !read_dpu_u64(dpu.dpu, observed_symbol, &observed))
				return false;
			if (pass == 1 && observed == pattern)
				++passed;
			else
				fprintf(stderr,
					"runtime validation failed ci=%u dpu=%u "
					"pass=%u observed=0x%lx\n",
					dpu.dpu->slice_id, dpu.dpu->dpu_id,
					pass, observed);
		}
	}

	printf("runtime heartbeat active on %u DPUs\n", heartbeats);
	if (mode == BF3_MUX_X4_RUNTIME)
		printf("X4 pattern observed on %u/%u target DPUs\n", passed,
		       checked);
	return heartbeats != 0 &&
	       (mode != BF3_MUX_X4_RUNTIME || (checked != 0 && passed == checked));
}

static bool verify_mux_is_dpu_side(struct dpu_set_t set)
{
	uint32_t checked = 0;
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		uint8_t value = 0;
		dpu_error_t status = dpu_wavegen_read_status(dpu.dpu, 2, &value);
		if (status != DPU_OK) {
			fprintf(stderr, "mux status read failed: %s\n",
				dpu_error_to_string(status));
			return false;
		}
		if ((value & 0x7b) != 0x03) {
			fprintf(stderr,
				"mux is not DPU-side at ci=%u dpu=%u: 0x%02x\n",
				dpu.dpu->slice_id, dpu.dpu->dpu_id, value);
			return false;
		}
		++checked;
	}
	printf("host SDK independently verified DPU-side mux on %u DPUs\n",
	       checked);
	return checked != 0;
}

static bool compare_x1_snapshot(const bf3_mux_config &config,
				const bf3_mux_result &result)
{
	for (uint32_t ci = 0; ci < config.nr_cis; ++ci) {
		if (!(config.ci_mask & (1u << ci)))
			continue;
		if (config.host_response[ci] != result.response_before[ci]) {
			fprintf(stderr,
				"X1 mismatch CI%u host=0x%016lx BF3=0x%016lx\n",
				ci, config.host_response[ci],
				result.response_before[ci]);
			return false;
		}
	}
	return true;
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

	printf("Starting %s iterations=%u pair=%u timeout_us=%u\n",
	       mode_name(options.mode), options.iterations, options.pair_index,
	       options.timeout_us);

	struct dpu_set_t set;
	struct dpu_program_t *program = nullptr;
	DPU_ASSERT(dpu_alloc(options.num_dpus, options.profile.c_str(), &set));
	struct dpu_rank_t *rank = first_rank(set);
	if (rank == nullptr) {
		dpu_free(set);
		return 1;
	}
	uint64_t rank_base = rank_base_address(rank);

	bool runtime_mode = options.mode == BF3_MUX_X4_RUNTIME ||
			    options.mode == BF3_MUX_X6_STRESS;
	struct dpu_symbol_t pattern_symbol = {};
	struct dpu_symbol_t gate_symbol = {};
	const uint64_t pattern = 0xb3f11a5a600d0004ULL;
	if (runtime_mode) {
		DPU_ASSERT(dpu_load(set, DPU_BINARY, &program));
		uint32_t zero = 0;
		uint32_t mode = options.mode;
		DPU_ASSERT(dpu_copy_to(set, "test_mode", 0, &mode,
				       sizeof(mode)));
		DPU_ASSERT(dpu_copy_to(set, "stop", 0, &zero, sizeof(zero)));
		DPU_ASSERT(dpu_copy_to(set, "pass", 0, &zero, sizeof(zero)));
		DPU_ASSERT(dpu_copy_to(set, "heartbeat", 0, &zero,
				       sizeof(zero)));
		DPU_ASSERT(dpu_copy_to(set, "check_pattern", 0, &zero,
				       sizeof(zero)));
		DPU_ASSERT(dpu_copy_to(set, "expected_pattern", 0, &pattern,
				       sizeof(pattern)));
		DPU_ASSERT(dpu_get_symbol(program, "mux_pattern",
					  &pattern_symbol));
		DPU_ASSERT(dpu_get_symbol(program, "check_pattern",
					  &gate_symbol));
	}

	if (options.mode >= BF3_MUX_X3_FLIP)
		DPU_ASSERT(dpu_switch_mux_for_rank(rank, false));
	if (runtime_mode && !boot_without_polling(rank)) {
		dpu_free(set);
		return 1;
	}

	uint64_t host_response[BF3_MUX_NR_CIS] = {};
	if (ci_get_color(rank, nullptr) != DPU_OK ||
	    ci_update_commands(rank, host_response) != DPU_OK) {
		fprintf(stderr, "failed to snapshot CI color/response\n");
		if (runtime_mode)
			stop_runtime(set, rank);
		dpu_free(set);
		return 1;
	}
	uint8_t next_color = GET_CI_CONTEXT(rank)->color;
	uint8_t nr_cis =
		rank->description->hw.topology.nr_of_control_interfaces;
	uint8_t ci_mask = nr_cis >= 8 ? 0xffu :
				    static_cast<uint8_t>((1u << nr_cis) - 1u);
	flush_ci_lines(rank_base);

	bf3_mux_config config = {};
	config.magic = BF3_MUX_MAGIC;
	config.version = BF3_MUX_VERSION;
	config.mode = options.mode;
	config.ci_mask = ci_mask;
	config.iterations = options.iterations;
	config.timeout_us = options.timeout_us;
	config.pair_index = options.pair_index;
	config.rank_base = rank_base;
	config.command_addr = rank_base + BF3_MUX_COMMAND_OFFSET;
	config.response_addr = rank_base + BF3_MUX_RESPONSE_OFFSET;
	config.pattern_value = pattern;
	config.gate_wram_word_addr = gate_symbol.address >> 2;
	config.next_color = next_color;
	config.nr_cis = nr_cis;
	memcpy(config.host_response, host_response, sizeof(host_response));
	if (runtime_mode) {
		uint32_t logical_offset =
			mux_mram_logical_offset(pattern_symbol.address);
		config.pattern_addr =
			rank_base + mux_group_base_offset(logical_offset);
		printf("runtime pattern symbol=0x%x logical=0x%x remote=0x%lx\n",
		       pattern_symbol.address, logical_offset,
		       config.pattern_addr);
		printf("runtime WRAM gate symbol=0x%x word=0x%x "
		       "(released directly by BF3 CI)\n",
		       gate_symbol.address, config.gate_wram_word_addr);
	}

	Exporter exporter = {};
	exporter.control_fd = -1;
	if (!init_exporter(&exporter, options,
			   reinterpret_cast<void *>(rank_base),
			   256ul * 1024ul * 1024ul)) {
		if (runtime_mode) {
			flush_ci_lines(rank_base);
			ci_get_color(rank, nullptr);
			stop_runtime(set, rank);
		}
		dpu_free(set);
		return 1;
	}

	printf("CI ownership handoff host->BF3 next_color=0x%02x "
	       "command=0x%lx response=0x%lx\n",
	       next_color, config.command_addr, config.response_addr);
	fflush(stdout);

	bool ok = send_all(exporter.control_fd, &config, sizeof(config));
	bf3_mux_result result = {};
	if (ok)
		ok = recv_all(exporter.control_fd, &result, sizeof(result));
	if (!ok) {
		fprintf(stderr, "BF3 control exchange failed\n");
	} else if (result.magic != BF3_MUX_MAGIC ||
		   result.version != BF3_MUX_VERSION ||
		   result.mode != options.mode) {
		fprintf(stderr, "invalid BF3 result header\n");
		ok = false;
	} else if (result.status != BF3_MUX_STATUS_OK) {
		fprintf(stderr, "BF3 reported status=%u after %lu operations\n",
			result.status, result.completed);
		ok = false;
	}

	/*
	 * BF3 sends the result only after its last synchronous CQE, so no more
	 * CI DMA can race this resynchronization point.
	 */
	flush_ci_lines(rank_base);
	uint32_t color_status = ci_get_color(rank, nullptr);
	if (color_status != DPU_OK) {
		fprintf(stderr, "host color resync failed: %s\n",
			dpu_error_to_string(
				static_cast<dpu_error_t>(color_status)));
		ok = false;
	}
	printf("CI ownership handoff BF3->host BF3_next_color=0x%02x "
	       "host_next_color=0x%02x\n",
	       result.final_next_color, GET_CI_CONTEXT(rank)->color);

	if (ok && options.mode == BF3_MUX_X1_READ_RESPONSE)
		ok = compare_x1_snapshot(config, result);

	if (ok && options.mode >= BF3_MUX_X3_FLIP)
		ok = verify_mux_is_dpu_side(set);

	if (runtime_mode) {
		bool stopped = stop_runtime(set, rank);
		bool runtime_ok =
			stopped && validate_runtime(set, program, options.mode,
						    pattern);
		ok = ok && runtime_ok;
	}

	printf("BF3 stats completed=%lu ci_commands=%lu reads=%lu writes=%lu "
	       "decode=%lu collision=%lu elapsed_ms=%.3f "
	       "p50_us=%.3f p99_us=%.3f max_us=%.3f\n",
	       result.completed, result.ci_commands, result.dma_reads,
	       result.dma_writes, result.decode_faults,
	       result.collision_faults, result.elapsed_ns / 1e6,
	       result.p50_ns / 1e3, result.p99_ns / 1e3,
	       result.max_ns / 1e3);
	printf("%s %s\n", mode_name(options.mode), ok ? "PASS" : "FAIL");

	destroy_exporter(&exporter);
	DPU_ASSERT(dpu_free(set));
	return ok && !stop_requested ? 0 : 1;
}
