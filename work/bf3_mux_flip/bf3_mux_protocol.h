#ifndef BF3_MUX_PROTOCOL_H
#define BF3_MUX_PROTOCOL_H

#include <stdint.h>

#define BF3_MUX_MAGIC 0x584d3342u /* "B3MX" */
#define BF3_MUX_VERSION 2u
#define BF3_MUX_NR_CIS 8u
#define BF3_MUX_COMMAND_OFFSET 0x20000u
#define BF3_MUX_RESPONSE_OFFSET 0x28000u
#define BF3_MUX_PATTERN_BYTES 128u

enum bf3_mux_mode {
	BF3_MUX_X1_READ_RESPONSE = 1,
	BF3_MUX_X2_IDENTITY = 2,
	BF3_MUX_X3_FLIP = 3,
	BF3_MUX_X4_RUNTIME = 4,
	BF3_MUX_X5_BENCH = 5,
	BF3_MUX_X6_STRESS = 6,
};

enum bf3_mux_status {
	BF3_MUX_STATUS_OK = 0,
	BF3_MUX_STATUS_BAD_CONFIG = 1,
	BF3_MUX_STATUS_DMA_ERROR = 2,
	BF3_MUX_STATUS_CI_TIMEOUT = 3,
	BF3_MUX_STATUS_CI_FAULT = 4,
	BF3_MUX_STATUS_MUX_MISMATCH = 5,
	BF3_MUX_STATUS_PATTERN_ERROR = 6,
};

struct bf3_mux_config {
	uint32_t magic;
	uint32_t version;
	uint32_t mode;
	uint32_t ci_mask;
	uint32_t iterations;
	uint32_t timeout_us;
	uint32_t pair_index;
	uint32_t gate_wram_word_addr;
	uint64_t rank_base;
	uint64_t command_addr;
	uint64_t response_addr;
	uint64_t pattern_addr;
	uint64_t pattern_value;
	uint64_t host_response[BF3_MUX_NR_CIS];
	uint8_t next_color;
	uint8_t nr_cis;
	uint8_t reserved1[6];
};

struct bf3_mux_result {
	uint32_t magic;
	uint32_t version;
	uint32_t mode;
	uint32_t status;
	uint64_t completed;
	uint64_t dma_reads;
	uint64_t dma_writes;
	uint64_t ci_commands;
	uint64_t decode_faults;
	uint64_t collision_faults;
	uint64_t elapsed_ns;
	uint64_t p50_ns;
	uint64_t p99_ns;
	uint64_t max_ns;
	uint64_t response_before[BF3_MUX_NR_CIS];
	uint64_t response_after[BF3_MUX_NR_CIS];
	uint8_t final_next_color;
	uint8_t final_mux_status[BF3_MUX_NR_CIS];
	uint8_t reserved[7];
};

#endif
