#include <arpa/inet.h>
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
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <dpu.h>
#include <dpu_management.h>
#include <dpu_memory.h>
#include <dpu_program.h>
#include <dpu_rank.h>
#include "../ufi/include/ufi/ufi_config.h"
}

#include "../bfdma/libr.hpp"
#include "../pimnic_bf3_runtime/control_protocol.h"
#include "../pimnic_bf3_runtime/mram_addr.h"
#include "../pimnic_bf3_runtime/mram_guard.h"
#include "../pimnic_bf3_runtime/ring_layout.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./build/example/bf_checksum_runtime"
#endif

static bool stop_flag;

static void ctrl_c_handler(int)
{
	stop_flag = true;
}

struct Options {
	uint32_t num_dpus = 64;
	uint32_t active_dpus = 0;
	uint32_t group_id = 0;
	uint32_t payload = PIMNIC_RUNTIME_DEFAULT_PAYLOAD;
	uint32_t payload_offset = 0;
	uint32_t iterations = PIMNIC_RUNTIME_DEFAULT_ITERATIONS;
	int port = 6666;
	int numa_node = 0;
	std::string device_name = "mlx5_0";
	std::string bf3_ip;
};

struct LaneInfo {
	uint8_t lane = 0;
	uint8_t dpu_id = 0;
	uint8_t slice_id = 0;
	struct dpu_t *dpu = nullptr;
};

struct RankInfo {
	struct dpu_rank_t *rank = nullptr;
	uint64_t base_addr = 0;
};

struct GroupInfo {
	RankInfo rank;
	uint32_t group_in_rank = 0;
	LaneInfo lanes[PIMNIC_PE_GROUP_SIZE];
};

struct RuntimeExporter {
	NetParam net_param;
	vhca_resource resource;
	int control_fd = -1;
};

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--num-dpus N] [--group-id G] [--payload BYTES] "
		"[--active-dpus N] [--iterations N] [--port P] "
		"[--device-name DEV] [--numa-node N] [--bf3-ip IP] "
		"[--payload-offset BYTES]\n",
		argv0);
}

static bool parse_u32(const char *value, uint32_t *out)
{
	char *end = nullptr;
	unsigned long parsed = strtoul(value, &end, 0);
	if (end == value || *end != '\0')
		return false;
	*out = (uint32_t)parsed;
	return true;
}

static bool parse_i32(const char *value, int *out)
{
	char *end = nullptr;
	long parsed = strtol(value, &end, 0);
	if (end == value || *end != '\0')
		return false;
	*out = (int)parsed;
	return true;
}

static bool parse_options(int argc, char **argv, Options *options)
{
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			usage(argv[0]);
			return false;
		}
		if (i + 1 >= argc) {
			fprintf(stderr, "missing value for %s\n", arg.c_str());
			return false;
		}
		const char *value = argv[++i];
		if (arg == "--num-dpus") {
			if (!parse_u32(value, &options->num_dpus))
				return false;
		} else if (arg == "--active-dpus") {
			if (!parse_u32(value, &options->active_dpus))
				return false;
		} else if (arg == "--group-id") {
			if (!parse_u32(value, &options->group_id))
				return false;
		} else if (arg == "--payload") {
			if (!parse_u32(value, &options->payload))
				return false;
		} else if (arg == "--payload-offset") {
			if (!parse_u32(value, &options->payload_offset))
				return false;
		} else if (arg == "--iterations") {
			if (!parse_u32(value, &options->iterations))
				return false;
		} else if (arg == "--port") {
			if (!parse_i32(value, &options->port))
				return false;
		} else if (arg == "--numa-node") {
			if (!parse_i32(value, &options->numa_node))
				return false;
		} else if (arg == "--device-name") {
			options->device_name = value;
		} else if (arg == "--bf3-ip") {
			options->bf3_ip = value;
		} else {
			fprintf(stderr, "unknown option %s\n", arg.c_str());
			return false;
		}
	}

	return true;
}

static bool send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *ptr = (const uint8_t *)buf;
	while (len > 0) {
		ssize_t sent = send(fd, ptr, len, 0);
		if (sent < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (sent == 0)
			return false;
		ptr += sent;
		len -= (size_t)sent;
	}
	return true;
}

static bool recv_all(int fd, void *buf, size_t len)
{
	uint8_t *ptr = (uint8_t *)buf;
	while (len > 0) {
		ssize_t got = recv(fd, ptr, len, MSG_WAITALL);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (got == 0)
			return false;
		ptr += got;
		len -= (size_t)got;
	}
	return true;
}

static uint32_t expected_checksum(uint64_t epoch, uint8_t lane,
				  uint32_t per_lane_bytes)
{
	(void)epoch;
	(void)lane;
	return per_lane_bytes;
}

static bool env_enabled(const char *name)
{
	const char *value = getenv(name);
	return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
}

static uint32_t env_u32(const char *name, uint32_t default_value)
{
	const char *value = getenv(name);
	if (value == nullptr || value[0] == '\0')
		return default_value;
	char *end = nullptr;
	unsigned long parsed = strtoul(value, &end, 0);
	if (end == value || *end != '\0')
		return default_value;
	return (uint32_t)parsed;
}

static uint64_t rank_base_addr(struct dpu_rank_t *rank)
{
	dpu_rank_handler_t handler = rank->handler_context->handler;
	return handler->__get_base_region_address(rank);
}

static bool collect_ranks_and_lanes(struct dpu_set_t set,
				    std::vector<RankInfo> *ranks,
				    std::map<struct dpu_rank_t *,
					     std::map<uint32_t, struct dpu_t *>>
					    *dpus_by_rank)
{
	struct dpu_set_t dpu;

	DPU_FOREACH(set, dpu)
	{
		struct dpu_rank_t *rank = dpu.dpu->rank;
		uint32_t pe_index = dpu.dpu->dpu_id * 8u + dpu.dpu->slice_id;

		if (dpus_by_rank->find(rank) == dpus_by_rank->end()) {
			RankInfo info;
			info.rank = rank;
			info.base_addr = rank_base_addr(rank);
			ranks->push_back(info);
		}
		(*dpus_by_rank)[rank][pe_index] = dpu.dpu;
	}

	std::sort(ranks->begin(), ranks->end(),
		  [](const RankInfo &a, const RankInfo &b) {
			  return a.base_addr < b.base_addr;
		  });

	return !ranks->empty();
}

static uint32_t required_allocated_pes_for_groups(uint32_t first_group,
						  uint32_t group_count)
{
	uint32_t required = 0;

	for (uint32_t index = 0; index < group_count; ++index) {
		uint32_t group_in_rank = (first_group + index) % 4u;
		uint32_t upper_last_pe = 32u + group_in_rank * 8u + 7u;
		if (upper_last_pe + 1u > required)
			required = upper_last_pe + 1u;
	}

	return required;
}

static bool build_group(struct dpu_set_t set, uint32_t group_id,
			GroupInfo *group)
{
	std::vector<RankInfo> ranks;
	std::map<struct dpu_rank_t *, std::map<uint32_t, struct dpu_t *>>
		dpus_by_rank;

	if (!collect_ranks_and_lanes(set, &ranks, &dpus_by_rank))
		return false;

	uint32_t rank_id = group_id / 4u;
	if (rank_id >= ranks.size()) {
		fprintf(stderr, "group %u requires rank %u, but only %zu rank(s) "
				"were allocated\n",
			group_id, rank_id, ranks.size());
		return false;
	}

	group->rank = ranks[rank_id];
	group->group_in_rank = group_id % 4u;

	const std::map<uint32_t, struct dpu_t *> &rank_dpus =
		dpus_by_rank[group->rank.rank];
	for (uint8_t lane = 0; lane < PIMNIC_PE_GROUP_SIZE; ++lane) {
		uint32_t pe_index = lane < 8u ?
					    group->group_in_rank * 8u + lane :
					    32u + group->group_in_rank * 8u +
						    (lane - 8u);
		auto it = rank_dpus.find(pe_index);
		if (it == rank_dpus.end()) {
			fprintf(stderr, "group %u lane %u requires PE %u, which "
					"was not allocated\n",
				group_id, lane, pe_index);
			return false;
		}
		group->lanes[lane].lane = lane;
		group->lanes[lane].dpu_id = (uint8_t)(pe_index / 8u);
		group->lanes[lane].slice_id = (uint8_t)(pe_index % 8u);
		group->lanes[lane].dpu = it->second;
	}

	printf("Selected group %u: rank_base=0x%lx group_in_rank=%u\n",
	       group_id, group->rank.base_addr, group->group_in_rank);
	for (uint8_t lane = 0; lane < PIMNIC_PE_GROUP_SIZE; ++lane)
		printf("  lane %u -> dpu_id=%u slice_id=%u\n", lane,
		       group->lanes[lane].dpu_id, group->lanes[lane].slice_id);

	return true;
}

static bool init_exporter(RuntimeExporter *exporter, const Options &options,
			  void *buffer, uint64_t size)
{
	signal(SIGINT, ctrl_c_handler);
	signal(SIGTERM, ctrl_c_handler);

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

	struct devx_hca_capabilities caps;
	if (devx_query_hca_caps(exporter->net_param.contexts[0], &caps) != 0) {
		fprintf(stderr, "devx_query_hca_caps failed\n");
		return false;
	}

	printf("pim1 vhca_id=%u crossing_vhca_mkey_supported=%d\n",
	       caps.vhca_id, caps.crossing_vhca_mkey_supported);

	exporter->resource.pd = ibv_alloc_pd(exporter->net_param.contexts[0]);
	if (exporter->resource.pd == nullptr) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		return false;
	}

	exporter->resource.vhca_id = caps.vhca_id;
	exporter->resource.addr = buffer;
	exporter->resource.size = size;
	exporter->resource.mr =
		devx_reg_mr(exporter->resource.pd, exporter->resource.addr,
			    exporter->resource.size,
			    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				    IBV_ACCESS_REMOTE_WRITE |
				    IBV_ACCESS_RELAXED_ORDERING);
	if (exporter->resource.mr == nullptr) {
		fprintf(stderr, "devx_reg_mr failed\n");
		return false;
	}
	exporter->resource.mkey = devx_mr_query_mkey(exporter->resource.mr);

	uint8_t access_key[32] = { 0 };
	for (uint8_t &byte : access_key)
		byte = 1;
	if (devx_mr_allow_other_vhca_access(exporter->resource.mr, access_key,
					    sizeof(access_key)) != 0) {
		fprintf(stderr, "devx_mr_allow_other_vhca_access failed\n");
		return false;
	}

	printf("Registered rank MR addr=%p size=%lu mkey=%u\n",
	       exporter->resource.addr, exporter->resource.size,
	       exporter->resource.mkey);
	socket_init(exporter->net_param);
	exchange_vhca_data(exporter->net_param, &exporter->resource, 1);
	exporter->control_fd = exporter->net_param.sockfd[1];
	printf("BF3 control channel connected on fd=%d\n", exporter->control_fd);
	fflush(stdout);

	return true;
}

static bool write_lane_u32(const LaneInfo &lane, struct dpu_symbol_t symbol,
			   uint32_t value)
{
	dpuword_t word = value;
	return dpu_copy_to_wram_for_dpu(lane.dpu, symbol.address >> 2, &word,
					1) == DPU_OK;
}

static bool read_lane_u32(const LaneInfo &lane, struct dpu_symbol_t symbol,
			  uint32_t *value)
{
	dpuword_t word = 0;
	dpu_error_t status =
		dpu_copy_from_wram_for_dpu(lane.dpu, &word, symbol.address >> 2,
					   1);
	*value = word;
	return status == DPU_OK;
}

static bool notify_group(const GroupInfo &group, struct dpu_symbol_t rx_len,
			 struct dpu_symbol_t rx_offset,
			 struct dpu_symbol_t rx_ready, uint32_t seq,
			 uint32_t payload, uint32_t payload_offset)
{
	for (const LaneInfo &lane : group.lanes) {
		if (!write_lane_u32(lane, rx_offset, payload_offset))
			return false;
		if (!write_lane_u32(lane, rx_len, payload))
			return false;
		if (!write_lane_u32(lane, rx_ready, seq))
			return false;
	}
	return true;
}

static bool wait_group_done(const GroupInfo &group, struct dpu_symbol_t done,
			    struct dpu_symbol_t checksum, uint32_t seq,
			    uint32_t payload)
{
	bool lane_complete[PIMNIC_PE_GROUP_SIZE] = { false };
	uint32_t complete = 0;

	for (uint32_t round = 0; round < 50000 && complete < PIMNIC_PE_GROUP_SIZE;
	     ++round) {
		for (uint8_t lane_id = 0; lane_id < PIMNIC_PE_GROUP_SIZE;
		     ++lane_id) {
			if (lane_complete[lane_id])
				continue;
			uint32_t done_value = 0;
			if (!read_lane_u32(group.lanes[lane_id], done,
					   &done_value))
				return false;
			if (done_value == seq) {
				lane_complete[lane_id] = true;
				++complete;
			}
		}
		if (complete == PIMNIC_PE_GROUP_SIZE)
			break;
		usleep(100);
	}

	if (complete != PIMNIC_PE_GROUP_SIZE) {
		fprintf(stderr, "timeout waiting for done seq=%u (%u/%u lanes)\n",
			seq, complete, PIMNIC_PE_GROUP_SIZE);
		return false;
	}

	bool all_match = true;
	bool verbose_checksum = env_enabled("PIMNIC_VERBOSE_CHECKSUM");
	for (uint8_t lane_id = 0; lane_id < PIMNIC_PE_GROUP_SIZE; ++lane_id) {
		uint32_t actual = 0;
		uint32_t expected = expected_checksum(seq, lane_id, payload);
		if (!read_lane_u32(group.lanes[lane_id], checksum, &actual))
			return false;
		if (verbose_checksum || actual != expected)
			printf("seq %u lane %u checksum actual=%u expected=%u\n",
			       seq, lane_id, actual, expected);
		if (actual != expected) {
			fprintf(stderr,
				"checksum mismatch seq=%u lane=%u expected=%u "
				"actual=%u\n",
				seq, lane_id, expected, actual);
			all_match = false;
		}
	}
	fflush(stdout);

	return all_match;
}

static uint32_t checksum_host_group_mapping(const GroupInfo &group,
					    uint32_t payload_mram_offset,
					    uint32_t per_lane_bytes)
{
	uint8_t *rank_base = (uint8_t *)group.rank.base_addr;
	uint32_t total = 0;

	for (uint8_t lane_id = 0; lane_id < PIMNIC_PE_GROUP_SIZE; ++lane_id) {
		uint32_t lane_sum = 0;
		for (uint32_t byte = 0; byte < per_lane_bytes; ++byte) {
			uint64_t lane_offset = pimnic_group_lane_offset(
				group.group_in_rank, lane_id,
				payload_mram_offset + byte);
			lane_sum += rank_base[lane_offset];
		}
		uint64_t first_lane_offset = pimnic_group_lane_offset(
			group.group_in_rank, lane_id, payload_mram_offset);
		printf("host mapping lane %u first_off=0x%lx first=%u sum=%u\n",
		       lane_id, first_lane_offset, rank_base[first_lane_offset],
		       lane_sum);
		total += lane_sum;
	}
	fflush(stdout);

	return total;
}

static void host_rewrite_group_mapping(uint64_t remote_addr,
				       uint32_t group_bytes)
{
	uint8_t *ptr = (uint8_t *)remote_addr;
	memset(ptr, 1, group_bytes);
	for (uint32_t offset = 0; offset < group_bytes; offset += 64)
		_mm_clflush(ptr + offset);
	_mm_mfence();
}

static void flush_host_mapping_range(uint64_t remote_addr, uint32_t bytes)
{
	uint8_t *ptr = (uint8_t *)remote_addr;
	for (uint32_t offset = 0; offset < bytes; offset += 64)
		_mm_clflush(ptr + offset);
	_mm_mfence();
}

static void settle_dma_window(void)
{
	uint32_t settle_us = env_u32("PIMNIC_DMA_SETTLE_US", 100);
	if (settle_us != 0)
		usleep(settle_us);
}

static bool send_dma_epoch(RuntimeExporter *exporter, uint64_t epoch,
			   uint64_t remote_addr, uint32_t per_lane_bytes,
			   uint32_t group_bytes)
{
	bool verbose_runtime = env_enabled("PIMNIC_VERBOSE_RUNTIME");
	pimnic_dma_req req = {};
	req.opcode = PIMNIC_CTRL_DMA_WRITE_REQ;
	req.version = PIMNIC_CTRL_VERSION;
	req.epoch = epoch;
	req.remote_addr = remote_addr;
	req.per_lane_bytes = per_lane_bytes;
	req.group_bytes = group_bytes;

	if (!send_all(exporter->control_fd, &req, sizeof(req))) {
		fprintf(stderr, "failed to send DMA request for epoch %lu\n",
			epoch);
		return false;
	}
	if (verbose_runtime || epoch == 1 || epoch % 1000u == 0) {
		printf("seq %lu DMA request sent\n", epoch);
		fflush(stdout);
	}

	pimnic_dma_ack ack = {};
	if (!recv_all(exporter->control_fd, &ack, sizeof(ack))) {
		fprintf(stderr, "failed to receive DMA ack for epoch %lu\n",
			epoch);
		return false;
	}

	if (ack.opcode != PIMNIC_CTRL_DMA_ACK || ack.epoch != epoch ||
	    ack.status != 0 || ack.bytes != group_bytes) {
		fprintf(stderr,
			"bad DMA ack: opcode=%u status=%u epoch=%lu bytes=%u\n",
			ack.opcode, ack.status, ack.epoch, ack.bytes);
		return false;
	}
	if (verbose_runtime || epoch == 1 || epoch % 1000u == 0) {
		printf("seq %lu DMA ack received bytes=%u\n", epoch, ack.bytes);
		fflush(stdout);
	}

	return true;
}

static uint64_t dma_epoch_id(uint32_t seq, uint32_t group_id,
			     uint32_t chunk_index)
{
	return ((uint64_t)seq << 32) | ((uint64_t)group_id << 16) |
	       chunk_index;
}

static void send_shutdown(RuntimeExporter *exporter)
{
	if (exporter->control_fd < 0)
		return;
	pimnic_dma_req req = {};
	req.opcode = PIMNIC_CTRL_SHUTDOWN;
	req.version = PIMNIC_CTRL_VERSION;
	send_all(exporter->control_fd, &req, sizeof(req));
}

int main(int argc, char **argv)
{
	Options options;
	if (!parse_options(argc, argv, &options))
		return 2;

	if (options.num_dpus == 0 || options.num_dpus % PIMNIC_PE_GROUP_SIZE) {
		fprintf(stderr, "--num-dpus must be a nonzero multiple of %u\n",
			PIMNIC_PE_GROUP_SIZE);
		return 2;
	}
	if (options.active_dpus != 0 &&
	    (options.active_dpus % PIMNIC_PE_GROUP_SIZE) != 0) {
		fprintf(stderr,
			"--active-dpus must be a multiple of %u when set\n",
			PIMNIC_PE_GROUP_SIZE);
		return 2;
	}
	if (options.payload == 0 ||
	    options.payload > PIMNIC_RUNTIME_PAYLOAD_MAX) {
		fprintf(stderr, "--payload must be in 1..%u\n",
			PIMNIC_RUNTIME_PAYLOAD_MAX);
		return 2;
	}
	if (options.payload_offset > PIMNIC_RUNTIME_PAYLOAD_MAX ||
	    options.payload > PIMNIC_RUNTIME_PAYLOAD_MAX -
			      options.payload_offset) {
		fprintf(stderr, "--payload-offset + --payload must be <= %u\n",
			PIMNIC_RUNTIME_PAYLOAD_MAX);
		return 2;
	}

	uint32_t active_dpus =
		options.active_dpus == 0 ? PIMNIC_PE_GROUP_SIZE :
					   options.active_dpus;
	uint32_t group_count = active_dpus / PIMNIC_PE_GROUP_SIZE;
	uint32_t alloc_dpus = options.num_dpus;
	if (options.active_dpus != 0) {
		uint32_t required_dpus = required_allocated_pes_for_groups(
			options.group_id, group_count);
		if (alloc_dpus < required_dpus)
			alloc_dpus = required_dpus;
		if (alloc_dpus % PIMNIC_PE_GROUP_SIZE)
			alloc_dpus = ((alloc_dpus + PIMNIC_PE_GROUP_SIZE - 1u) /
				      PIMNIC_PE_GROUP_SIZE) *
				     PIMNIC_PE_GROUP_SIZE;
	}
	printf("Requested num_dpus=%u active_dpus=%u group_count=%u "
	       "allocating=%u first_group=%u\n",
	       options.num_dpus, active_dpus, group_count, alloc_dpus,
	       options.group_id);
	fflush(stdout);

	struct dpu_set_t set;
	struct dpu_program_t *program = nullptr;
	DPU_ASSERT(dpu_alloc(alloc_dpus, NULL, &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY, &program));

	uint32_t nr_dpus = 0;
	DPU_ASSERT(dpu_get_nr_dpus(set, &nr_dpus));
	printf("Allocated DPUs = %u\n", nr_dpus);

	std::vector<GroupInfo> groups(group_count);
	for (uint32_t group_index = 0; group_index < group_count;
	     ++group_index) {
		uint32_t global_group_id = options.group_id + group_index;
		if (!build_group(set, global_group_id, &groups[group_index])) {
			dpu_free(set);
			return 1;
		}
		if (groups[group_index].rank.rank != groups[0].rank.rank) {
			fprintf(stderr,
				"multi-rank active test is not supported yet: "
				"group %u is on a different rank\n",
				global_group_id);
			dpu_free(set);
			return 1;
		}
	}

	struct dpu_symbol_t rx_payload_symbol;
	struct dpu_symbol_t rx_ready_symbol;
	struct dpu_symbol_t rx_len_symbol;
	struct dpu_symbol_t rx_offset_symbol;
	struct dpu_symbol_t stop_symbol;
	struct dpu_symbol_t done_symbol;
	struct dpu_symbol_t checksum_symbol;
	struct dpu_symbol_t error_symbol;
	DPU_ASSERT(dpu_get_symbol(program, "rx_payload", &rx_payload_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "rx_ready", &rx_ready_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "rx_len", &rx_len_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "rx_offset", &rx_offset_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "stop", &stop_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "done", &done_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "checksum", &checksum_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "error_code", &error_symbol));

	uint32_t zero = 0;
	DPU_ASSERT(dpu_copy_to(set, "rx_ready", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "rx_len", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "rx_offset", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "stop", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "done", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "checksum", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "error_code", 0, &zero, sizeof(zero)));

	uint32_t payload_mram_offset =
		pimnic_mram_logical_offset(rx_payload_symbol.address) +
		options.payload_offset;
	uint32_t group_bytes = pimnic_group_dma_bytes(options.payload);

	printf("rx_payload symbol=0x%x logical_mram_offset=0x%x "
	       "per_lane=%u group_bytes=%u\n",
	       rx_payload_symbol.address, payload_mram_offset, options.payload,
	       group_bytes);

	DPU_ASSERT(dpu_switch_mux_for_rank(groups[0].rank.rank, true));

	RuntimeExporter exporter;
	memset(&exporter, 0, sizeof(exporter));
	exporter.control_fd = -1;
	if (!init_exporter(&exporter, options, (void *)groups[0].rank.base_addr,
			   256ul * 1024ul * 1024ul)) {
		dpu_free(set);
		return 1;
	}

	printf("Launching persistent DPU kernel asynchronously\n");
	fflush(stdout);
	DPU_ASSERT(dpu_launch(set, DPU_ASYNCHRONOUS));
	printf("DPU launch returned\n");
	fflush(stdout);

	bool ok = true;
	for (uint32_t seq = 1; seq <= options.iterations && !stop_flag; ++seq) {
		bool log_seq = env_enabled("PIMNIC_VERBOSE_RUNTIME") ||
			       seq == 1 || seq == options.iterations ||
			       seq % 1000u == 0;
		for (uint32_t group_index = 0; group_index < group_count;
		     ++group_index) {
			uint32_t global_group_id = options.group_id + group_index;
			GroupInfo &group = groups[group_index];
			bool chunked_dma =
				group.group_in_rank == 0 &&
				options.payload > PIMNIC_PE_LANE_BYTES &&
				(options.payload_offset %
				 PIMNIC_PE_LANE_BYTES) == 0;
			uint64_t remote_addr =
				group.rank.base_addr +
				pimnic_group_base_offset(
					group.group_in_rank,
					payload_mram_offset);
			bool dma_ok = true;

			if (chunked_dma) {
				if (log_seq) {
					printf("seq %u group %u using 8B chunked mram guard\n",
					       seq, global_group_id);
					fflush(stdout);
				}
				uint32_t chunk_index = 0;
				for (uint32_t chunk_offset = 0;
				     chunk_offset < options.payload;
				     chunk_offset += PIMNIC_PE_LANE_BYTES,
					     ++chunk_index) {
					uint32_t chunk_payload =
						options.payload - chunk_offset;
					if (chunk_payload > PIMNIC_PE_LANE_BYTES)
						chunk_payload =
							PIMNIC_PE_LANE_BYTES;
					uint32_t chunk_mram_offset =
						payload_mram_offset +
						chunk_offset;
					uint64_t chunk_remote_addr =
						group.rank.base_addr +
						pimnic_group_base_offset(
							group.group_in_rank,
							chunk_mram_offset);
					uint32_t chunk_group_bytes =
						pimnic_group_dma_bytes(
							chunk_payload);

					if (log_seq) {
						printf("seq %u group %u chunk offset=%u begin mram guard\n",
						       seq, global_group_id,
						       chunk_offset);
						fflush(stdout);
					}
					dpu_error_t guard_status =
						pimnic_group_external_mram_dma_begin(
							group.rank.rank,
							group.group_in_rank);
					if (guard_status != DPU_OK) {
						fprintf(stderr,
							"mram dma begin failed at seq=%u group=%u chunk=%u: %s\n",
							seq, global_group_id,
							chunk_offset,
							dpu_error_to_string(
								guard_status));
						ok = false;
						break;
					}

				dma_ok = send_dma_epoch(
					&exporter,
					dma_epoch_id(seq,
						     global_group_id,
						     chunk_index),
					chunk_remote_addr,
					chunk_payload,
					chunk_group_bytes);
				if (dma_ok) {
					flush_host_mapping_range(chunk_remote_addr,
								 chunk_group_bytes);
					settle_dma_window();
				}

				if (log_seq) {
					printf("seq %u group %u chunk offset=%u end mram guard\n",
						       seq, global_group_id,
						       chunk_offset);
						fflush(stdout);
					}
					dpu_error_t end_status =
						pimnic_group_external_mram_dma_end(
							group.rank.rank,
							group.group_in_rank);
					if (end_status != DPU_OK) {
						fprintf(stderr,
							"mram dma end failed at seq=%u group=%u chunk=%u: %s\n",
							seq, global_group_id,
							chunk_offset,
							dpu_error_to_string(
								end_status));
						ok = false;
						break;
					}
					if (!dma_ok)
						break;
					settle_dma_window();
				}
				if (!ok || !dma_ok) {
					ok = false;
					break;
				}
				if (seq == 1 ||
				    env_enabled("PIMNIC_DIAG_READBACK")) {
					uint32_t mapping_sum =
						checksum_host_group_mapping(
							group,
							payload_mram_offset,
							options.payload);
					printf("seq %u group %u host mapping total checksum=%u expected=%u\n",
					       seq, global_group_id,
					       mapping_sum,
					       options.payload *
						       PIMNIC_PE_GROUP_SIZE);
					fflush(stdout);
				}
			} else {
				if (log_seq) {
					printf("seq %u group %u begin mram guard\n",
					       seq, global_group_id);
					fflush(stdout);
				}
				dpu_error_t guard_status =
					pimnic_group_external_mram_dma_begin(
						group.rank.rank,
						group.group_in_rank);
				if (guard_status != DPU_OK) {
					fprintf(stderr,
						"mram dma begin failed at seq=%u group=%u: %s\n",
						seq, global_group_id,
						dpu_error_to_string(
							guard_status));
					ok = false;
					break;
				}
				if (log_seq) {
					printf("seq %u group %u mram guard begin done\n",
					       seq, global_group_id);
					fflush(stdout);
				}

				dma_ok = send_dma_epoch(
					&exporter,
					dma_epoch_id(seq, global_group_id, 0),
					remote_addr, options.payload,
					group_bytes);
				if (dma_ok) {
					flush_host_mapping_range(remote_addr,
								 group_bytes);
					settle_dma_window();
				}
				if (dma_ok &&
				    (seq == 1 ||
				     env_enabled("PIMNIC_DIAG_READBACK"))) {
					uint32_t mapping_sum =
						checksum_host_group_mapping(
							group,
							payload_mram_offset,
							options.payload);
					printf("seq %u group %u host mapping total checksum=%u expected=%u\n",
					       seq, global_group_id,
					       mapping_sum,
					       options.payload *
						       PIMNIC_PE_GROUP_SIZE);
					fflush(stdout);
					if (getenv("PIMNIC_DEBUG_HOST_REWRITE") !=
					    nullptr) {
						host_rewrite_group_mapping(
							remote_addr,
							group_bytes);
						printf("seq %u group %u host rewrite after DMA done\n",
						       seq,
						       global_group_id);
						fflush(stdout);
					}
				}

				if (log_seq) {
					printf("seq %u group %u end mram guard\n",
					       seq, global_group_id);
					fflush(stdout);
				}
				dpu_error_t end_status =
					pimnic_group_external_mram_dma_end(
						group.rank.rank,
						group.group_in_rank);
				if (end_status != DPU_OK) {
					fprintf(stderr,
						"mram dma end failed at seq=%u group=%u: %s\n",
						seq, global_group_id,
						dpu_error_to_string(
							end_status));
					ok = false;
					break;
				}
				if (log_seq) {
					printf("seq %u group %u mram guard end done\n",
					       seq, global_group_id);
					fflush(stdout);
				}
				settle_dma_window();
				if (!dma_ok) {
					ok = false;
					break;
				}
			}

			if (log_seq) {
				printf("seq %u group %u notify WRAM\n", seq,
				       global_group_id);
				fflush(stdout);
			}
			if (!notify_group(group, rx_len_symbol, rx_offset_symbol,
					  rx_ready_symbol, seq, options.payload,
					  options.payload_offset)) {
				fprintf(stderr,
					"WRAM notify failed at seq=%u group=%u\n",
					seq, global_group_id);
				ok = false;
				break;
			}

			if (log_seq) {
				printf("seq %u group %u wait DPU done\n", seq,
				       global_group_id);
				fflush(stdout);
			}
			if (!wait_group_done(group, done_symbol, checksum_symbol,
					     seq, options.payload)) {
				ok = false;
				break;
			}
		}

		if (!ok)
			break;
		if (seq == 1 || seq == options.iterations || seq % 1000u == 0)
			printf("seq %u/%u active_dpus=%u passed\n", seq,
			       options.iterations, active_dpus);
	}

	send_shutdown(&exporter);
	uint32_t one = 1;
	dpuword_t stop_word = one;
	struct dpu_set_t stop_dpu;
	DPU_FOREACH(set, stop_dpu)
	{
		DPU_ASSERT(dpu_copy_to_wram_for_dpu(
			stop_dpu.dpu, stop_symbol.address >> 2, &stop_word, 1));
	}
	DPU_ASSERT(dpu_sync(set));

	if (!ok) {
		for (uint32_t group_index = 0; group_index < group_count;
		     ++group_index) {
			uint32_t global_group_id = options.group_id + group_index;
			for (const LaneInfo &lane : groups[group_index].lanes) {
				uint32_t error = 0;
				read_lane_u32(lane, error_symbol, &error);
				fprintf(stderr,
					"group %u lane %u error_code=%u\n",
					global_group_id, lane.lane, error);
			}
		}
	}

	DPU_ASSERT(dpu_free(set));
	printf("bf_checksum_runtime_host %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
