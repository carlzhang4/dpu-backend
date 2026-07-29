#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

extern "C" {
#include <dpu.h>
#include <dpu_management.h>
#include <dpu_rank.h>
}

#include "../pimnic_bf3_runtime/mram_addr.h"
#include "../pimnic_bf3_runtime/mram_guard.h"
#include "../pimnic_bf3_runtime/ring_layout.h"
#include "../pimnic_bf3_runtime/test_pattern.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./build/example/bf_pimnic_runtime"
#endif

struct Options {
	uint32_t rank = 0;
	uint32_t group = 0;
	uint32_t length = 4096;
	uint32_t inject_shift = 0;
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

static bool parse_options(int argc, char **argv, Options *options)
{
	for (int index = 1; index < argc; ++index) {
		if (index + 1 == argc)
			return false;
		std::string argument = argv[index];
		const char *value = argv[++index];
		if (argument == "--rank") {
			if (!parse_u32(value, &options->rank))
				return false;
		} else if (argument == "--group") {
			if (!parse_u32(value, &options->group))
				return false;
		} else if (argument == "--len") {
			if (!parse_u32(value, &options->length))
				return false;
		} else if (argument == "--inject-shift") {
			if (!parse_u32(value, &options->inject_shift))
				return false;
		} else {
			return false;
		}
	}
	return options->rank < 8 && options->group < 4 &&
	       options->length >= PIMNIC_PATTERN_HEADER_BYTES +
					  PIMNIC_PATTERN_CRC_BYTES &&
	       options->length <= PIMNIC_RUNTIME_PAYLOAD_MAX &&
	       options->inject_shift < options->length;
}

static uint64_t rank_base_address(struct dpu_rank_t *rank)
{
	dpu_rank_handler_t handler = rank->handler_context->handler;
	return handler->__get_base_region_address(rank);
}

static void deinterleave(
	const std::vector<uint8_t> &physical, uint32_t per_lane_bytes,
	std::array<std::vector<uint8_t>, PIMNIC_PE_GROUP_SIZE> *lanes)
{
	for (uint32_t lane = 0; lane < PIMNIC_PE_GROUP_SIZE; ++lane) {
		(*lanes)[lane].resize(per_lane_bytes);
		uint32_t half = lane / 8u;
		uint32_t lane_in_half = lane % 8u;
		for (uint32_t byte = 0; byte < per_lane_bytes; ++byte) {
			uint32_t unit = byte / 8u;
			uint32_t byte_in_unit = byte % 8u;
			(*lanes)[lane][byte] =
				physical[unit *
						 PIMNIC_PE_GROUP_UNIT_BYTES +
					 half * 64u +
					 byte_in_unit * 8u +
					 lane_in_half];
		}
	}
}

static bool direct_read_group(
	struct dpu_rank_t *rank, uint32_t group, uint32_t logical_offset,
	uint32_t per_lane_bytes,
	std::array<std::vector<uint8_t>, PIMNIC_PE_GROUP_SIZE> *lanes)
{
	uint32_t units =
		(per_lane_bytes + PIMNIC_PE_LANE_BYTES - 1u) /
		PIMNIC_PE_LANE_BYTES;
	std::vector<uint8_t> physical(
		units * PIMNIC_PE_GROUP_UNIT_BYTES, 0);
	uint64_t base = rank_base_address(rank);

	for (uint32_t unit = 0; unit < units; ++unit) {
		uint8_t *source = reinterpret_cast<uint8_t *>(
			base + pimnic_group_base_offset(
				       group,
				       logical_offset + unit * 8u));
		for (uint32_t line = 0;
		     line < PIMNIC_PE_GROUP_UNIT_BYTES; line += 64)
			_mm_clflushopt(source + line);
		_mm_mfence();
		memcpy(physical.data() +
			       unit * PIMNIC_PE_GROUP_UNIT_BYTES,
		       source, PIMNIC_PE_GROUP_UNIT_BYTES);
	}
	deinterleave(physical, per_lane_bytes, lanes);
	return true;
}

int main(int argc, char **argv)
{
	Options options;
	if (!parse_options(argc, argv, &options)) {
		fprintf(stderr,
			"Usage: %s [--rank N] [--group 0..3] "
			"[--len 20..4096] [--inject-shift N]\n",
			argv[0]);
		return 2;
	}

	struct dpu_set_t set;
	struct dpu_program_t *program = nullptr;
	DPU_ASSERT(dpu_alloc((options.rank + 1u) * 64u, "backend=hw",
			     &set));
	DPU_ASSERT(dpu_load(set, DPU_BINARY, &program));

	std::vector<struct dpu_rank_t *> ranks;
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		if (std::find(ranks.begin(), ranks.end(),
			      dpu.dpu->rank) == ranks.end())
			ranks.push_back(dpu.dpu->rank);
	}
	std::sort(ranks.begin(), ranks.end(),
		  [](const struct dpu_rank_t *left,
		     const struct dpu_rank_t *right) {
			  return rank_base_address(
					 const_cast<struct dpu_rank_t *>(
						 left)) <
				 rank_base_address(
					 const_cast<struct dpu_rank_t *>(
						 right));
		  });
	if (options.rank >= ranks.size()) {
		dpu_free(set);
		return 1;
	}
	struct dpu_rank_t *rank = ranks[options.rank];

	struct dpu_symbol_t rx_data = {};
	DPU_ASSERT(dpu_get_symbol(program, "rx_data", &rx_data));
	uint32_t logical_offset =
		pimnic_mram_logical_offset(rx_data.address);

	std::array<std::vector<uint8_t>, PIMNIC_PE_GROUP_SIZE> sdk;
	uint32_t written = 0;
	DPU_FOREACH(set, dpu)
	{
		if (dpu.dpu->rank != rank)
			continue;
		uint32_t group =
			dpu.dpu->dpu_id < 4 ?
				dpu.dpu->dpu_id :
				dpu.dpu->dpu_id - 4;
		if (group != options.group)
			continue;
		uint32_t lane =
			dpu.dpu->dpu_id < 4 ?
				dpu.dpu->slice_id :
				8u + dpu.dpu->slice_id;
		std::vector<uint8_t> expected(options.length);
		std::vector<uint8_t> write_buffer(options.length, 0);
		pimnic_fill_pattern(expected.data(), expected.size(),
				    0x1001u + lane,
				    static_cast<uint8_t>(lane));
		memcpy(write_buffer.data() + options.inject_shift,
		       expected.data(),
		       options.length - options.inject_shift);
		DPU_ASSERT(dpu_copy_to(dpu, "rx_data", 0,
				       write_buffer.data(),
				       write_buffer.size()));
		sdk[lane].resize(options.length);
		DPU_ASSERT(dpu_copy_from(dpu, "rx_data", 0,
					 sdk[lane].data(),
					 sdk[lane].size()));
		++written;
	}
	if (written != PIMNIC_PE_GROUP_SIZE) {
		fprintf(stderr, "expected 16 lanes, wrote %u\n", written);
		dpu_free(set);
		return 1;
	}

	if (pimnic_group_external_mram_dma_begin(rank, options.group) !=
	    DPU_OK) {
		fprintf(stderr, "cannot switch requested group to host side\n");
		dpu_free(set);
		return 1;
	}
	std::array<std::vector<uint8_t>, PIMNIC_PE_GROUP_SIZE> direct;
	bool direct_ok = direct_read_group(
		rank, options.group, logical_offset, options.length, &direct);
	dpu_error_t close_status =
		pimnic_group_external_mram_dma_end(rank, options.group);

	uint32_t failed = 0;
	for (uint32_t lane = 0; lane < PIMNIC_PE_GROUP_SIZE; ++lane) {
		if (!direct_ok || direct[lane] != sdk[lane]) {
			fprintf(stderr,
				"dual-read mismatch rank=%u group=%u lane=%u\n",
				options.rank, options.group, lane);
			++failed;
			continue;
		}
		uint8_t expected_byte = 0;
		uint8_t actual_byte = 0;
		int64_t bad = pimnic_verify_pattern(
			direct[lane].data(), options.length,
			0x1001u + lane, static_cast<uint8_t>(lane),
			&expected_byte, &actual_byte);
		int64_t wanted =
			options.inject_shift == 0 ? -1 : 0;
		if (bad != wanted) {
			fprintf(stderr,
				"verify mismatch lane=%u first_bad=%ld "
				"wanted=%ld expected=0x%02x actual=0x%02x\n",
				lane, bad, wanted, expected_byte,
				actual_byte);
			++failed;
		}
	}

	printf("mram-readback %s rank=%u group=%u lanes=16 len=%u "
	       "inject_shift=%u expected_first_bad=%d\n",
	       failed == 0 && close_status == DPU_OK ? "PASS" : "FAIL",
	       options.rank, options.group, options.length,
	       options.inject_shift, options.inject_shift == 0 ? -1 : 0);
	DPU_ASSERT(dpu_free(set));
	return failed == 0 && close_status == DPU_OK ? 0 : 1;
}
