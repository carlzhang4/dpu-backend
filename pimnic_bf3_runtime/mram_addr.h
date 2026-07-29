#ifndef PIMNIC_BF3_MRAM_ADDR_H
#define PIMNIC_BF3_MRAM_ADDR_H

#include <stdint.h>

#define PIMNIC_MRAM_SYMBOL_MASK 0x08000000u
#define PIMNIC_PE_GROUP_SIZE 16u
#define PIMNIC_PE_LANE_BYTES 8u
#define PIMNIC_PE_GROUP_UNIT_BYTES 128u
#define PIMNIC_BANK_CHUNK_BYTES 0x20000u
#define PIMNIC_BANK_NEXT_CHUNK_BYTES 0x100000u
#define PIMNIC_PE_GROUP_WINDOW_BYTES 0x40000u
#define PIMNIC_RANK_WINDOW_BYTES PIMNIC_BANK_NEXT_CHUNK_BYTES

uint32_t pimnic_mram_logical_offset(uint32_t symbol_addr);
uint32_t pimnic_mram_translate_offset(uint32_t logical_offset);
uint64_t pimnic_group_base_offset(uint32_t group_in_rank,
				  uint32_t pe_mram_offset);
uint64_t pimnic_group_lane_offset(uint32_t group_in_rank, uint8_t lane,
				  uint32_t pe_mram_offset);
uint32_t pimnic_group_dma_bytes(uint32_t per_lane_bytes);

#endif
