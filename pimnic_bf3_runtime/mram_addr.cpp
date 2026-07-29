#include "mram_addr.h"

uint32_t pimnic_mram_logical_offset(uint32_t symbol_addr)
{
	return symbol_addr & ~PIMNIC_MRAM_SYMBOL_MASK;
}

uint32_t pimnic_mram_translate_offset(uint32_t logical_offset)
{
	uint32_t mask_21_to_15 = ((1u << (21 - 15 + 1)) - 1u) << 15;
	uint32_t mask_21_to_14 = ((1u << (21 - 14 + 1)) - 1u) << 14;
	uint32_t bits_21_to_15 = (logical_offset & mask_21_to_15) >> 15;
	uint32_t bit_14 = (logical_offset >> 14) & 1u;
	uint32_t unchanged_bits = logical_offset & ~mask_21_to_14;

	return unchanged_bits | (bits_21_to_15 << 14) | (bit_14 << 21);
}

uint64_t pimnic_group_base_offset(uint32_t group_in_rank,
				  uint32_t pe_mram_offset)
{
	uint32_t translated = pimnic_mram_translate_offset(pe_mram_offset);
	uint64_t unit_index = translated / PIMNIC_PE_LANE_BYTES;
	uint64_t true_byte = unit_index * PIMNIC_PE_GROUP_UNIT_BYTES;

	return (true_byte % PIMNIC_BANK_CHUNK_BYTES) +
	       (true_byte / PIMNIC_BANK_CHUNK_BYTES) *
		       PIMNIC_BANK_NEXT_CHUNK_BYTES +
	       (uint64_t)group_in_rank * PIMNIC_PE_GROUP_WINDOW_BYTES;
}

uint64_t pimnic_group_lane_offset(uint32_t group_in_rank, uint8_t lane,
				  uint32_t pe_mram_offset)
{
	uint32_t translated = pimnic_mram_translate_offset(pe_mram_offset);
	uint64_t byte_in_lane = translated % PIMNIC_PE_LANE_BYTES;
	uint64_t half_select = lane / 8u;
	uint64_t lane_in_half = lane % 8u;

	return pimnic_group_base_offset(group_in_rank, pe_mram_offset) +
	       half_select * 64u + byte_in_lane * 8u + lane_in_half;
}

uint32_t pimnic_group_dma_bytes(uint32_t per_lane_bytes)
{
	uint32_t unit_count = (per_lane_bytes + PIMNIC_PE_LANE_BYTES - 1u) /
			      PIMNIC_PE_LANE_BYTES;
	return unit_count * PIMNIC_PE_GROUP_UNIT_BYTES;
}
