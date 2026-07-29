#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <x86intrin.h>

#include <map>
#include <string>
#include <vector>

extern "C" {
#include <dpu.h>
#include <dpu_error.h>
#include <dpu_management.h>
#include <dpu_memory.h>
#include <dpu_program.h>
#include <dpu_rank.h>
#include "../ufi/include/ufi/ufi_config.h"
}

#include "../pimnic_bf3_runtime/mram_addr.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./build/example/bf_checksum_runtime"
#endif

struct Options {
	uint32_t num_dpus = 64;
	uint32_t payload = 32;
	uint32_t payload_offset = 0;
	uint32_t iterations = 1;
	uint32_t dpu_id = UINT32_MAX;
	bool dump_units = false;
	bool split_units = false;
};

struct DpuLine {
	uint8_t dpu_id = 0;
	struct dpu_t *slices[8] = {};
};

static bool parse_u32(const char *value, uint32_t *out)
{
	char *end = nullptr;
	unsigned long parsed = strtoul(value, &end, 0);
	if (end == value || *end != '\0')
		return false;
	*out = (uint32_t)parsed;
	return true;
}

static bool parse_options(int argc, char **argv, Options *options)
{
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--dump-units") {
			options->dump_units = true;
			continue;
		}
		if (arg == "--split-units") {
			options->split_units = true;
			continue;
		}
		if (i + 1 >= argc)
			return false;
		const char *value = argv[++i];
		if (arg == "--num-dpus") {
			if (!parse_u32(value, &options->num_dpus))
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
		} else if (arg == "--dpu-id") {
			if (!parse_u32(value, &options->dpu_id))
				return false;
		} else {
			return false;
		}
	}
	return true;
}

static uint64_t rank_base_addr(struct dpu_rank_t *rank)
{
	dpu_rank_handler_t handler = rank->handler_context->handler;
	return handler->__get_base_region_address(rank);
}

static bool write_wram_u32(struct dpu_t *dpu, struct dpu_symbol_t symbol,
			   uint32_t value)
{
	dpuword_t word = value;
	return dpu_copy_to_wram_for_dpu(dpu, symbol.address >> 2, &word, 1) ==
	       DPU_OK;
}

static bool read_wram_u32(struct dpu_t *dpu, struct dpu_symbol_t symbol,
			  uint32_t *value)
{
	dpuword_t word = 0;
	dpu_error_t status =
		dpu_copy_from_wram_for_dpu(dpu, &word, symbol.address >> 2, 1);
	*value = word;
	return status == DPU_OK;
}

static bool read_wram_words(struct dpu_t *dpu, struct dpu_symbol_t symbol,
			    uint32_t word_offset, std::vector<dpuword_t> *words)
{
	if (words->empty())
		return true;
	dpu_error_t status = dpu_copy_from_wram_for_dpu(
		dpu, words->data(), (symbol.address >> 2) + word_offset,
		words->size());
	return status == DPU_OK;
}

static uint64_t dpu_lane_offset(uint8_t dpu_id, uint8_t slice_id,
				uint32_t pe_mram_offset)
{
	uint32_t group_in_rank = dpu_id % 4u;
	uint8_t lane = (dpu_id >= 4u) ? (uint8_t)(8u + slice_id) : slice_id;
	return pimnic_group_lane_offset(group_in_rank, lane, pe_mram_offset);
}

static void write_payload_to_line(uint64_t rank_base, uint8_t dpu_id,
				  uint32_t payload_mram_offset,
				  uint32_t payload)
{
	uint8_t *base = (uint8_t *)rank_base;

	for (uint8_t slice_id = 0; slice_id < 8; ++slice_id) {
		for (uint32_t byte = 0; byte < payload; ++byte) {
			uint64_t offset = dpu_lane_offset(
				dpu_id, slice_id, payload_mram_offset + byte);
			base[offset] = 1;
			_mm_clflush(base + offset);
		}
	}
	_mm_mfence();
}

static void print_host_line_sum(uint64_t rank_base, uint8_t dpu_id,
				uint32_t payload_mram_offset,
				uint32_t payload)
{
	uint8_t *base = (uint8_t *)rank_base;

	for (uint8_t slice_id = 0; slice_id < 8; ++slice_id) {
		uint32_t sum = 0;
		uint64_t first_offset =
			dpu_lane_offset(dpu_id, slice_id, payload_mram_offset);
		for (uint32_t byte = 0; byte < payload; ++byte) {
			uint64_t offset = dpu_lane_offset(
				dpu_id, slice_id, payload_mram_offset + byte);
			sum += base[offset];
		}
		printf("host dpu_id=%u slice=%u first_off=0x%lx first=%u sum=%u\n",
		       dpu_id, slice_id, first_offset, base[first_offset], sum);
	}
	fflush(stdout);
}

static bool wait_line_done(const DpuLine &line, struct dpu_symbol_t done,
			   struct dpu_symbol_t checksum,
			   struct dpu_symbol_t unit_checksum, uint32_t seq,
			   uint32_t payload_offset, uint32_t payload,
			   bool dump_units)
{
	bool complete[8] = {};
	uint32_t count = 0;

	for (uint32_t round = 0; round < 50000 && count < 8; ++round) {
		for (uint8_t slice_id = 0; slice_id < 8; ++slice_id) {
			if (complete[slice_id])
				continue;
			uint32_t done_value = 0;
			if (!read_wram_u32(line.slices[slice_id], done,
					   &done_value))
				return false;
			if (done_value == seq) {
				complete[slice_id] = true;
				++count;
			}
		}
		if (count == 8)
			break;
		usleep(100);
	}

	if (count != 8) {
		fprintf(stderr, "timeout dpu_id=%u seq=%u done=%u/8\n",
			line.dpu_id, seq, count);
		return false;
	}

	bool ok = true;
	uint32_t first_unit = payload_offset / PIMNIC_PE_LANE_BYTES;
	uint32_t last_unit =
		(payload_offset + payload - 1) / PIMNIC_PE_LANE_BYTES;
	uint32_t unit_count = last_unit - first_unit + 1;
	for (uint8_t slice_id = 0; slice_id < 8; ++slice_id) {
		uint32_t actual = 0;
		if (!read_wram_u32(line.slices[slice_id], checksum, &actual))
			return false;
		printf("dpu_id=%u slice=%u checksum actual=%u expected=%u\n",
		       line.dpu_id, slice_id, actual, payload);
		if (actual != payload)
			ok = false;
		if (dump_units || actual != payload) {
			std::vector<dpuword_t> unit_sums(unit_count);
			if (!read_wram_words(line.slices[slice_id],
					     unit_checksum, first_unit,
					     &unit_sums))
				return false;
			printf("dpu_id=%u slice=%u unit_base=%u unit_sums=",
			       line.dpu_id, slice_id, first_unit);
			for (uint32_t unit = 0; unit < unit_count; ++unit)
				printf("%s%u", unit == 0 ? "" : ",",
				       (uint32_t)unit_sums[unit]);
			printf("\n");
		}
	}
	fflush(stdout);

	return ok;
}

int main(int argc, char **argv)
{
	Options options;
	if (!parse_options(argc, argv, &options)) {
		fprintf(stderr,
			"Usage: %s [--num-dpus N] [--payload BYTES] "
			"[--payload-offset BYTES] [--iterations N] "
			"[--dpu-id ID] [--dump-units] [--split-units]\n",
			argv[0]);
		return 2;
	}
	if (options.payload == 0 || options.payload_offset > 4096 ||
	    options.payload > 4096 - options.payload_offset) {
		fprintf(stderr,
			"--payload must be nonzero and payload-offset + payload "
			"must be <= 4096\n");
		return 2;
	}
	if (options.dpu_id != UINT32_MAX && options.dpu_id > UINT8_MAX) {
		fprintf(stderr, "--dpu-id must fit in uint8_t\n");
		return 2;
	}
	if (options.split_units &&
	    (options.payload_offset % PIMNIC_PE_LANE_BYTES) != 0) {
		fprintf(stderr, "--split-units requires 8-byte aligned offset\n");
		return 2;
	}

	struct dpu_set_t set;
	struct dpu_program_t *program = nullptr;
	DPU_ASSERT(dpu_alloc(options.num_dpus, NULL, &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY, &program));

	std::map<uint32_t, DpuLine> lines;
	struct dpu_rank_t *rank = nullptr;
	struct dpu_set_t each;
	DPU_FOREACH(set, each)
	{
		if (rank == nullptr)
			rank = each.dpu->rank;
		if (each.dpu->rank != rank)
			continue;
		uint8_t dpu_id = each.dpu->dpu_id;
		uint8_t slice_id = each.dpu->slice_id;
		lines[dpu_id].dpu_id = dpu_id;
		lines[dpu_id].slices[slice_id] = each.dpu;
	}

	if (rank == nullptr) {
		fprintf(stderr, "no rank allocated\n");
		dpu_free(set);
		return 1;
	}

	std::vector<uint8_t> test_dpus;
	if (options.dpu_id == UINT32_MAX) {
		for (uint8_t dpu_id = 0; dpu_id < 8; ++dpu_id)
			test_dpus.push_back(dpu_id);
	} else {
		test_dpus.push_back((uint8_t)options.dpu_id);
	}
	for (uint8_t dpu_id : test_dpus) {
		if (lines.find(dpu_id) == lines.end()) {
			fprintf(stderr, "missing dpu_id=%u\n", dpu_id);
			dpu_free(set);
			return 1;
		}
		for (uint8_t slice_id = 0; slice_id < 8; ++slice_id) {
			if (lines[dpu_id].slices[slice_id] == nullptr) {
				fprintf(stderr, "missing dpu_id=%u slice=%u\n",
					dpu_id, slice_id);
				dpu_free(set);
				return 1;
			}
		}
	}

	struct dpu_symbol_t rx_payload_symbol;
	struct dpu_symbol_t rx_ready_symbol;
	struct dpu_symbol_t rx_len_symbol;
	struct dpu_symbol_t rx_offset_symbol;
	struct dpu_symbol_t stop_symbol;
	struct dpu_symbol_t done_symbol;
	struct dpu_symbol_t checksum_symbol;
	struct dpu_symbol_t unit_checksum_symbol;
	DPU_ASSERT(dpu_get_symbol(program, "rx_payload", &rx_payload_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "rx_ready", &rx_ready_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "rx_len", &rx_len_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "rx_offset", &rx_offset_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "stop", &stop_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "done", &done_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "checksum", &checksum_symbol));
	DPU_ASSERT(dpu_get_symbol(program, "unit_checksum",
				  &unit_checksum_symbol));

	uint32_t zero = 0;
	DPU_ASSERT(dpu_copy_to(set, "rx_ready", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "rx_len", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "rx_offset", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "stop", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "done", 0, &zero, sizeof(zero)));
	DPU_ASSERT(dpu_copy_to(set, "checksum", 0, &zero, sizeof(zero)));

	uint64_t rank_base = rank_base_addr(rank);
	uint32_t payload_mram_offset =
		pimnic_mram_logical_offset(rx_payload_symbol.address) +
		options.payload_offset;
	printf("rank_base=0x%lx rx_payload_offset=0x%x payload=%u "
	       "payload_offset=%u\n",
	       rank_base, payload_mram_offset, options.payload,
	       options.payload_offset);
	fflush(stdout);

	DPU_ASSERT(dpu_launch(set, DPU_ASYNCHRONOUS));

	bool ok = true;
	uint32_t seq = 1;
	for (uint32_t iteration = 0; iteration < options.iterations;
	     ++iteration) {
		for (uint8_t dpu_id : test_dpus) {
			printf("test seq=%u dpu_id=%u begin\n", seq, dpu_id);
			fflush(stdout);

			dpu_error_t status = DPU_OK;
			if (options.split_units) {
				for (uint32_t unit_offset = 0;
				     unit_offset < options.payload;
				     unit_offset += PIMNIC_PE_LANE_BYTES) {
					uint32_t unit_payload =
						options.payload - unit_offset;
					if (unit_payload > PIMNIC_PE_LANE_BYTES)
						unit_payload =
							PIMNIC_PE_LANE_BYTES;
					printf("test seq=%u dpu_id=%u split_offset=%u begin\n",
					       seq, dpu_id, unit_offset);
					fflush(stdout);
					status =
						fifo_dpu_switch_mux_for_dpu_line(
							rank, dpu_id, 0xff);
					if (status != DPU_OK) {
						fprintf(stderr,
							"fifo begin failed dpu_id=%u: %s\n",
							dpu_id,
							dpu_error_to_string(
								status));
						ok = false;
						goto stop;
					}
					write_payload_to_line(
						rank_base, dpu_id,
						payload_mram_offset +
							unit_offset,
						unit_payload);
					status =
						release_fifo_dpu_switch_mux_for_dpu_line(
							rank, dpu_id, 0xff);
					if (status != DPU_OK) {
						fprintf(stderr,
							"fifo end failed dpu_id=%u: %s\n",
							dpu_id,
							dpu_error_to_string(
								status));
						ok = false;
						goto stop;
					}
				}
			} else {
				status = fifo_dpu_switch_mux_for_dpu_line(
					rank, dpu_id, 0xff);
				if (status != DPU_OK) {
					fprintf(stderr,
						"fifo begin failed dpu_id=%u: %s\n",
						dpu_id,
						dpu_error_to_string(status));
					ok = false;
					goto stop;
				}
				write_payload_to_line(rank_base, dpu_id,
						      payload_mram_offset,
						      options.payload);
				status =
					release_fifo_dpu_switch_mux_for_dpu_line(
						rank, dpu_id, 0xff);
				if (status != DPU_OK) {
					fprintf(stderr,
						"fifo end failed dpu_id=%u: %s\n",
						dpu_id,
						dpu_error_to_string(status));
					ok = false;
					goto stop;
				}
			}
			print_host_line_sum(rank_base, dpu_id,
					    payload_mram_offset,
					    options.payload);

			for (uint8_t slice_id = 0; slice_id < 8; ++slice_id) {
				struct dpu_t *dpu = lines[dpu_id].slices[slice_id];
				if (!write_wram_u32(dpu, rx_offset_symbol,
						    options.payload_offset) ||
				    !write_wram_u32(dpu, rx_len_symbol,
						    options.payload) ||
				    !write_wram_u32(dpu, rx_ready_symbol, seq)) {
					fprintf(stderr,
						"notify failed dpu_id=%u slice=%u\n",
						dpu_id, slice_id);
					ok = false;
					goto stop;
				}
			}

			bool line_ok = wait_line_done(lines[dpu_id],
						      done_symbol,
						      checksum_symbol,
						      unit_checksum_symbol,
						      seq,
						      options.payload_offset,
						      options.payload,
						      options.dump_units);
			if (!line_ok)
				ok = false;

			printf("test seq=%u dpu_id=%u %s\n", seq, dpu_id,
			       line_ok ? "PASS" : "FAIL");
			fflush(stdout);
			++seq;
		}
	}

stop:
	{
		uint32_t one = 1;
		dpuword_t stop_word = one;
		struct dpu_set_t stop_dpu;
		DPU_FOREACH(set, stop_dpu)
		{
			DPU_ASSERT(dpu_copy_to_wram_for_dpu(
				stop_dpu.dpu, stop_symbol.address >> 2,
				&stop_word, 1));
		}
	}
	DPU_ASSERT(dpu_sync(set));
	DPU_ASSERT(dpu_free(set));

	printf("bf_checksum_runtime_host_only %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
