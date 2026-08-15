#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <map>
#include <new>
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

#include "libr.hpp"

#include "pimnic/host/ops_upmem.h"

namespace {
constexpr uint64_t kExportBytes = 512ULL * 1024ULL * 1024ULL;

struct RankInfo {
	dpu_rank_t *rank = nullptr;
	uint64_t base = 0;
};

struct UpmemSet {
	dpu_set_t set{};
	dpu_program_t *program = nullptr;
	uint32_t nr_pes = 0;
	pimnic_topology_t topology{};
	std::vector<RankInfo> ranks;
	std::map<std::string, dpu_symbol_t> symbols;
	std::vector<std::pair<uint32_t, pimnic_collective_spec_t>> collectives;
	pimnic_runtime_result last_result{};
};

struct UpmemExporter {
	UpmemSet *set = nullptr;
	NetParam net{};
	ibv_pd *pd = nullptr;
	std::vector<vhca_resource> resources;
	int control_fd = -1;
};

uint64_t rank_base_address(dpu_rank_t *rank)
{
	dpu_rank_handler_t handler = rank->handler_context->handler;
	return handler->__get_base_region_address(rank);
}

std::vector<RankInfo> collect_ranks(dpu_set_t set)
{
	std::vector<RankInfo> ranks;
	dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		dpu_rank_t *rank = dpu.dpu->rank;
		auto found = std::find_if(
			ranks.begin(), ranks.end(),
			[rank](const RankInfo &value) { return value.rank == rank; });
		if (found == ranks.end())
			ranks.push_back({ rank, rank_base_address(rank) });
	}
	std::sort(ranks.begin(), ranks.end(),
		  [](const RankInfo &left, const RankInfo &right) {
			  return left.base < right.base;
		  });
	return ranks;
}

int send_exact(int fd, const void *buffer, size_t bytes)
{
	const uint8_t *cursor = static_cast<const uint8_t *>(buffer);
	while (bytes != 0) {
		ssize_t sent = send(fd, cursor, bytes, MSG_NOSIGNAL);
		if (sent < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (sent == 0)
			return -ECONNRESET;
		cursor += sent;
		bytes -= static_cast<size_t>(sent);
	}
	return 0;
}

int recv_exact(int fd, void *buffer, size_t bytes)
{
	uint8_t *cursor = static_cast<uint8_t *>(buffer);
	while (bytes != 0) {
		ssize_t received = recv(fd, cursor, bytes, MSG_WAITALL);
		if (received < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (received == 0)
			return -ECONNRESET;
		cursor += received;
		bytes -= static_cast<size_t>(received);
	}
	return 0;
}

int resolve(UpmemSet *set, const char *name, dpu_symbol_t *symbol)
{
	auto found = set->symbols.find(name);
	if (found != set->symbols.end()) {
		*symbol = found->second;
		return 0;
	}
	dpu_symbol_t value{};
	dpu_error_t status = dpu_get_symbol(set->program, name, &value);
	if (status != DPU_OK)
		return -ENOENT;
	set->symbols.emplace(name, value);
	*symbol = value;
	return 0;
}

int write_word(dpu_t *dpu, dpu_symbol_t symbol, uint32_t value)
{
	dpuword_t word = value;
	return dpu_copy_to_wram_for_dpu(dpu, symbol.address >> 2, &word, 1) ==
		       DPU_OK ?
		       0 :
		       -EIO;
}

int read_word(dpu_t *dpu, dpu_symbol_t symbol, uint32_t *value)
{
	dpuword_t word = 0;
	if (dpu_copy_from_wram_for_dpu(dpu, &word, symbol.address >> 2, 1) !=
	    DPU_OK)
		return -EIO;
	*value = word;
	return 0;
}

void flush_ci_lines(uint64_t rank_base)
{
	_mm_clflush(reinterpret_cast<void *>(rank_base +
					    PIMNIC_CI_COMMAND_OFFSET));
	_mm_clflush(reinterpret_cast<void *>(rank_base +
					    PIMNIC_CI_RESPONSE_OFFSET));
	_mm_mfence();
}

bool logical_pe_index(const UpmemSet *set, const dpu_set_t &dpu,
		      uint32_t *index)
{
	auto rank_it = std::find_if(
		set->ranks.begin(), set->ranks.end(),
		[dpu](const RankInfo &rank) { return rank.rank == dpu.dpu->rank; });
	if (rank_it == set->ranks.end() || index == nullptr)
		return false;
	uint32_t rank_index = static_cast<uint32_t>(rank_it - set->ranks.begin());
	uint32_t lane = dpu.dpu->dpu_id < 4 ? dpu.dpu->slice_id :
					       8u + dpu.dpu->slice_id;
	uint32_t group = dpu.dpu->dpu_id & 3u;
	*index = rank_index * 64u + group * 16u + lane;
	return *index < set->nr_pes;
}

int op_alloc(void *, uint32_t nr_pes, const pimnic_topology_t *topology,
	     const char *profile, void **out)
{
	if (out == nullptr || nr_pes == 0 || topology == nullptr ||
	    profile == nullptr)
		return -EINVAL;
	auto *set = new (std::nothrow) UpmemSet;
	if (set == nullptr)
		return -ENOMEM;
	if (dpu_alloc(nr_pes, profile, &set->set) != DPU_OK) {
		delete set;
		return -EIO;
	}
	set->nr_pes = nr_pes;
	set->topology = *topology;
	set->ranks = collect_ranks(set->set);
	if (set->ranks.size() != nr_pes / 64u) {
		dpu_free(set->set);
		delete set;
		return -EPROTO;
	}
	*out = set;
	return 0;
}

int op_load(void *, void *platform_set, const char *binary)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	if (set == nullptr || binary == nullptr)
		return -EINVAL;
	if (dpu_load(set->set, binary, &set->program) != DPU_OK)
		return -EIO;
	std::vector<uint8_t> zero_desc(PIMNIC_DESC_COUNT *
					       PIMNIC_DESC_ENTRY_BYTES);
	uint64_t zero64 = 0;
	if (dpu_copy_to(set->set, "rx_desc", 0, zero_desc.data(),
			zero_desc.size()) != DPU_OK ||
	    dpu_copy_to(set->set, "tx_desc", 0, zero_desc.data(),
			zero_desc.size()) != DPU_OK ||
	    dpu_copy_to(set->set, "pe_pub", 0, &zero64, sizeof(zero64)) !=
		    DPU_OK ||
	    dpu_copy_to(set->set, "nic_pub", 0, &zero64, sizeof(zero64)) !=
		    DPU_OK)
		return -EIO;

	dpu_symbol_t gate_command{}, gate_ack{}, stop{}, lane_id{}, pe_index{};
	if (resolve(set, "gate_command", &gate_command) != 0 ||
	    resolve(set, "gate_ack", &gate_ack) != 0 ||
	    resolve(set, "stop", &stop) != 0)
		return -ENOENT;
	bool has_lane_id = resolve(set, "lane_id", &lane_id) == 0;
	bool has_pe_index = resolve(set, "pe_index", &pe_index) == 0;
	dpu_set_t dpu;
	DPU_FOREACH(set->set, dpu)
	{
		uint32_t lane = dpu.dpu->dpu_id < 4 ? dpu.dpu->slice_id :
						       8u + dpu.dpu->slice_id;
		uint32_t global_pe = 0;
		if (!logical_pe_index(set, dpu, &global_pe))
			return -EPROTO;
		if (write_word(dpu.dpu, gate_command, 0) != 0 ||
		    write_word(dpu.dpu, gate_ack, 0) != 0 ||
		    write_word(dpu.dpu, stop, 0) != 0 ||
		    (has_lane_id && write_word(dpu.dpu, lane_id, lane) != 0) ||
		    (has_pe_index && write_word(dpu.dpu, pe_index, global_pe) != 0))
			return -EIO;
	}
	return 0;
}

int op_config_u32(void *, void *platform_set, const char *symbol,
		  const uint32_t *values, uint32_t nr_pes)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	if (set == nullptr || values == nullptr || nr_pes != set->nr_pes)
		return -EINVAL;
	dpu_symbol_t resolved{};
	int rc = resolve(set, symbol, &resolved);
	if (rc != 0)
		return rc;
	dpu_set_t dpu;
	DPU_FOREACH(set->set, dpu)
	{
		uint32_t index = 0;
		if (!logical_pe_index(set, dpu, &index))
			return -EPROTO;
		rc = write_word(dpu.dpu, resolved, values[index]);
		if (rc != 0)
			return rc;
	}
	return 0;
}

int op_boot(void *, void *platform_set)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	for (const RankInfo &rank : set->ranks)
		if (dpu_boot_rank(rank.rank) != DPU_OK)
			return -EIO;
	return 0;
}

int op_stop(void *, void *platform_set, uint32_t timeout_us)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	dpu_symbol_t stop{};
	int rc = resolve(set, "stop", &stop);
	if (rc != 0)
		return rc;
	dpu_set_t dpu;
	DPU_FOREACH(set->set, dpu)
		if (write_word(dpu.dpu, stop, 1) != 0)
			return -EIO;
	uint64_t tries = std::max<uint64_t>(1, timeout_us / 10u);
	for (const RankInfo &rank : set->ranks) {
		bool stopped = false;
		for (uint64_t retry = 0; retry < tries; ++retry) {
			dpu_lock_rank(rank.rank);
			dpu_error_t status = dpu_poll_rank(rank.rank);
			uint32_t running =
				dpu_get_run_context(rank.rank)->nb_dpu_running;
			dpu_unlock_rank(rank.rank);
			if (status != DPU_OK)
				return -EIO;
			if (running == 0) {
				stopped = true;
				break;
			}
			usleep(10);
		}
		if (!stopped)
			return -ETIMEDOUT;
	}
	return 0;
}

void op_free(void *, void *platform_set)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	if (set != nullptr) {
		dpu_free(set->set);
		delete set;
	}
}

int op_preload(void *, void *platform_set, const char *symbol,
	       uint32_t offset, const void *source, uint32_t bytes_per_pe,
	       const pimnic_dim_op_t *dims, uint32_t nr_dims)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	if (set == nullptr || source == nullptr || dims == nullptr ||
	    nr_dims == 0 || nr_dims != set->topology.nr_dims)
		return -EINVAL;
	for (uint32_t dim = 0; dim < nr_dims; ++dim)
		if (dims[dim].dim != dim)
			return -EINVAL;
	const char *target = symbol == nullptr ? DPU_MRAM_HEAP_POINTER_NAME :
						     symbol;
	dpu_set_t dpu;
	DPU_FOREACH(set->set, dpu)
	{
		uint32_t source_index = 0;
		uint32_t scatter_stride = 1;
		uint32_t remaining = 0;
		if (!logical_pe_index(set, dpu, &remaining))
			return -EPROTO;
		for (uint32_t dim = 0; dim < nr_dims; ++dim) {
			uint32_t dim_size = set->topology.dims[dim];
			uint32_t coordinate = remaining % dim_size;
			remaining /= dim_size;
			if (dims[dim].prim == PIMNIC_PRIM_SCATTER) {
				source_index += coordinate * scatter_stride;
				scatter_stride *= dim_size;
			} else if (dims[dim].prim != PIMNIC_PRIM_BROADCAST) {
				return -EINVAL;
			}
		}
		void *cursor = const_cast<uint8_t *>(
			static_cast<const uint8_t *>(source) +
			static_cast<size_t>(source_index) * bytes_per_pe);
		if (dpu_prepare_xfer(dpu, cursor) != DPU_OK)
			return -EIO;
	}
	return dpu_push_xfer(set->set, DPU_XFER_TO_DPU, target, offset,
			     bytes_per_pe, DPU_XFER_DEFAULT) == DPU_OK ?
		       0 :
		       -EIO;
}

int op_collective_define(void *, void *platform_set,
			 const pimnic_collective_spec_t *spec, uint32_t id)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	if (set == nullptr || spec == nullptr || set->collectives.size() >= 2)
		return -ENOSPC;
	set->collectives.push_back({ id, *spec });
	return 0;
}

int op_mailbox_read(void *, void *platform_set, uint32_t pe,
		    const char *symbol, uint32_t *value)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	dpu_symbol_t resolved{};
	int rc = resolve(set, symbol, &resolved);
	if (rc != 0)
		return rc;
	dpu_set_t dpu;
	DPU_FOREACH(set->set, dpu)
	{
		uint32_t index = 0;
		if (logical_pe_index(set, dpu, &index) && index == pe)
			return read_word(dpu.dpu, resolved, value);
	}
	return -ENOENT;
}

int op_mailbox_write(void *, void *platform_set, uint32_t pe,
		     const char *symbol, uint32_t value)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	dpu_symbol_t resolved{};
	int rc = resolve(set, symbol, &resolved);
	if (rc != 0)
		return rc;
	dpu_set_t dpu;
	DPU_FOREACH(set->set, dpu)
	{
		uint32_t index = 0;
		if (logical_pe_index(set, dpu, &index) && index == pe)
			return write_word(dpu.dpu, resolved, value);
	}
	return -ENOENT;
}

int op_export_open(void *user, void *platform_set, const void *,
		   void **platform_exporter, int *control_fd)
{
	auto *params = static_cast<pimnic_upmem_params_t *>(user);
	auto *set = static_cast<UpmemSet *>(platform_set);
	auto *exporter = new (std::nothrow) UpmemExporter;
	if (params == nullptr || set == nullptr || exporter == nullptr)
		return -EINVAL;
	exporter->set = set;
	exporter->net.numNodes = 2;
	exporter->net.nodeId = 0;
	exporter->net.device_name = params->device_name;
	exporter->net.numa_node = params->numa_node;
	exporter->net.sock_port = params->port;
	exporter->net.sockfd = new int[128];
	exporter->net.ib_port = 1;
	exporter->net.page_size = sysconf(_SC_PAGESIZE);
	exporter->net.cacheline_size = get_cache_line_size();
	roce_init(exporter->net, 1);
	devx_hca_capabilities capabilities{};
	if (devx_query_hca_caps(exporter->net.contexts[0], &capabilities) != 0)
		return -EIO;
	exporter->pd = ibv_alloc_pd(exporter->net.contexts[0]);
	if (exporter->pd == nullptr)
		return -errno;
	uint8_t access_key[32];
	memset(access_key, 1, sizeof(access_key));
	exporter->resources.resize(set->ranks.size());
	for (uint32_t index = 0; index < set->ranks.size(); ++index) {
		vhca_resource &resource = exporter->resources[index];
		resource.pd = exporter->pd;
		resource.vhca_id = capabilities.vhca_id;
		resource.addr = reinterpret_cast<void *>(set->ranks[index].base);
		resource.size = kExportBytes;
		resource.mr = devx_reg_mr(
			exporter->pd, resource.addr, resource.size,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
		if (resource.mr == nullptr)
			return -EIO;
		resource.mkey = devx_mr_query_mkey(resource.mr);
		if (devx_mr_allow_other_vhca_access(
			    resource.mr, access_key, sizeof(access_key)) != 0)
			return -EIO;
	}
	socket_init(exporter->net);
	pimnic_resource_hello hello{ PIMNIC_ABI_MAGIC, PIMNIC_ABI_VERSION,
				      static_cast<uint32_t>(set->ranks.size()), 0 };
	exporter->control_fd = exporter->net.sockfd[1];
	int rc = send_exact(exporter->control_fd, &hello, sizeof(hello));
	if (rc == 0)
		exchange_vhca_data(exporter->net, exporter->resources.data(),
				   exporter->resources.size());
	if (rc != 0)
		return rc;
	*platform_exporter = exporter;
	*control_fd = exporter->control_fd;
	return 0;
}

void op_export_close(void *, void *platform_exporter)
{
	auto *exporter = static_cast<UpmemExporter *>(platform_exporter);
	if (exporter == nullptr)
		return;
	if (exporter->control_fd >= 0)
		close(exporter->control_fd);
	for (vhca_resource &resource : exporter->resources)
		if (resource.mr != nullptr)
			devx_dereg_mr(resource.mr);
	if (exporter->pd != nullptr)
		ibv_dealloc_pd(exporter->pd);
	delete[] exporter->net.sockfd;
	delete exporter;
}

int op_handoff_build(void *, void *platform_set, void *,
		     pimnic_runtime_config *config)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	config->nr_ranks = static_cast<uint32_t>(set->ranks.size());
	dpu_symbol_t rx_desc{}, tx_desc{}, pe_pub{}, nic_pub{}, rx_data{},
		tx_data{}, gate_command{}, gate_ack{};
	const char *names[] = { "rx_desc", "tx_desc", "pe_pub", "nic_pub",
				"rx_data", "tx_data", "gate_command", "gate_ack" };
	dpu_symbol_t *symbols[] = { &rx_desc, &tx_desc, &pe_pub, &nic_pub,
				    &rx_data, &tx_data, &gate_command, &gate_ack };
	for (uint32_t i = 0; i < 8; ++i)
		if (resolve(set, names[i], symbols[i]) != 0)
			return -ENOENT;
	for (uint32_t index = 0; index < config->nr_ranks; ++index) {
		RankInfo &rank = set->ranks[index];
		uint64_t snapshot[8]{};
		if (ci_get_color(rank.rank, nullptr) != DPU_OK ||
		    ci_update_commands(rank.rank, snapshot) != DPU_OK)
			return -EIO;
		uint8_t nr_cis = rank.rank->description->hw.topology
					 .nr_of_control_interfaces;
		pimnic_rank_config &entry = config->ranks[index];
		entry.rank_base = rank.base;
		entry.command_addr = rank.base + PIMNIC_CI_COMMAND_OFFSET;
		entry.response_addr = rank.base + PIMNIC_CI_RESPONSE_OFFSET;
		memcpy(entry.host_response, snapshot, sizeof(snapshot));
		auto logical = [](uint32_t address) {
			return address & ~0x08000000u;
		};
		entry.rx_desc_offset = logical(rx_desc.address);
		entry.tx_desc_offset = logical(tx_desc.address);
		entry.pe_pub_offset = logical(pe_pub.address);
		entry.nic_pub_offset = logical(nic_pub.address);
		entry.rx_data_offset = logical(rx_data.address);
		entry.tx_data_offset = logical(tx_data.address);
		entry.gate_command_word_addr = gate_command.address >> 2;
		entry.gate_ack_word_addr = gate_ack.address >> 2;
		entry.next_color = GET_CI_CONTEXT(rank.rank)->color;
		entry.nr_cis = nr_cis;
		entry.ci_mask = nr_cis >= 8 ? 0xffu :
						  (1u << nr_cis) - 1u;
		flush_ci_lines(rank.base);
	}
	auto *app = reinterpret_cast<pimnic_app_runtime_config *>(
		config->app_config);
	if (app->magic == PIMNIC_APP_MAGIC) {
		app->nr_collectives =
			static_cast<uint32_t>(set->collectives.size());
		for (uint32_t i = 0; i < set->collectives.size(); ++i) {
			const auto &definition = set->collectives[i];
			const auto &spec = definition.second;
			auto &wire = app->collectives[i];
			wire.id = definition.first;
			wire.meta = (spec.dim & 0xffu) |
				    ((spec.prim & 0xffu) << 8) |
				    ((spec.root & 0xffu) << 16) |
				    ((spec.writeback != 0) ? (1u << 24) : 0);
			wire.bytes_per_pe = spec.bytes_per_pe;
			wire.tag = spec.tag;
		}
	}
	return 0;
}

int op_handoff_start(void *, void *platform_exporter,
		     const pimnic_runtime_config *config)
{
	auto *exporter = static_cast<UpmemExporter *>(platform_exporter);
	return send_exact(exporter->control_fd, config, sizeof(*config));
}

int op_group_active(void *, void *platform_exporter, uint32_t group,
		    int active)
{
	auto *exporter = static_cast<UpmemExporter *>(platform_exporter);
	pimnic_group_control control{ PIMNIC_ABI_MAGIC, PIMNIC_ABI_VERSION,
		active ? PIMNIC_GROUP_ACTIVATE : PIMNIC_GROUP_DEACTIVATE,
		group };
	return send_exact(exporter->control_fd, &control, sizeof(control));
}

int op_wait_result(void *, void *platform_exporter,
		   pimnic_runtime_result *result, int timeout_ms)
{
	auto *exporter = static_cast<UpmemExporter *>(platform_exporter);
	if (timeout_ms >= 0) {
		pollfd descriptor{ exporter->control_fd, POLLIN, 0 };
		int ready;
		do {
			ready = poll(&descriptor, 1, timeout_ms);
		} while (ready < 0 && errno == EINTR);
		if (ready <= 0)
			return ready == 0 ? -ETIMEDOUT : -errno;
	}
	int rc = recv_exact(exporter->control_fd, result, sizeof(*result));
	if (rc == 0) {
		exporter->set->last_result = *result;
		if (result->elapsed_ns == 0)
			return -EPROTO;
	}
	return rc;
}

int op_handoff_reclaim(void *, void *platform_set)
{
	auto *set = static_cast<UpmemSet *>(platform_set);
	for (uint32_t index = 0; index < set->ranks.size(); ++index) {
		flush_ci_lines(set->ranks[index].base);
		if (ci_get_color(set->ranks[index].rank, nullptr) != DPU_OK)
			return -EIO;
		if (set->last_result.magic == PIMNIC_ABI_MAGIC &&
		    set->last_result.final_next_color[index] !=
			    GET_CI_CONTEXT(set->ranks[index].rank)->color)
			return -EPROTO;
	}
	return 0;
}

const pimnic_host_ops kOps = {
	op_alloc,
	op_load,
	op_config_u32,
	op_boot,
	op_stop,
	op_free,
	op_preload,
	op_collective_define,
	op_mailbox_read,
	op_mailbox_write,
	op_export_open,
	op_export_close,
	op_handoff_build,
	op_handoff_start,
	op_group_active,
	op_wait_result,
	op_handoff_reclaim,
};
} // namespace

extern "C" const pimnic_host_ops *pimnic_upmem_host_ops(void)
{
	return &kOps;
}
