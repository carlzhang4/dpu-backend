#ifndef PIMNIC_BF3_CONTROL_PROTOCOL_H
#define PIMNIC_BF3_CONTROL_PROTOCOL_H

#include <stdint.h>

#include "ring_layout.h"

#define PIMNIC_CTRL_MAGIC 0x50494d43u /* "PIMC" */
#define PIMNIC_CTRL_VERSION 3u
#define PIMNIC_MAX_RANKS 8u
#define PIMNIC_GROUPS_PER_RANK 4u

#define PIMNIC_CI_COMMAND_OFFSET 0x20000u
#define PIMNIC_CI_RESPONSE_OFFSET 0x28000u

struct pimnic_resource_hello {
	uint32_t magic;
	uint32_t version;
	uint32_t nr_ranks;
	uint32_t reserved;
};

enum pimnic_ctrl_status {
	PIMNIC_STATUS_OK = 0,
	PIMNIC_STATUS_BAD_CONFIG = 1,
	PIMNIC_STATUS_DMA_ERROR = 2,
	PIMNIC_STATUS_CI_ERROR = 3,
	PIMNIC_STATUS_GATE_TIMEOUT = 4,
	PIMNIC_STATUS_PATTERN_ERROR = 5,
	PIMNIC_STATUS_ORDER_ERROR = 6,
	PIMNIC_STATUS_TIMEOUT = 7,
};

enum pimnic_group_control_opcode {
	PIMNIC_GROUP_ACTIVATE = 1,
	PIMNIC_GROUP_DEACTIVATE = 2,
};

struct pimnic_group_control {
	uint32_t magic;
	uint32_t version;
	uint32_t opcode;
	uint32_t group;
};

struct pimnic_rank_config {
	uint64_t rank_base;
	uint64_t command_addr;
	uint64_t response_addr;
	uint64_t host_response[8];
	uint32_t rx_desc_offset;
	uint32_t tx_desc_offset;
	uint32_t pe_pub_offset;
	uint32_t nic_pub_offset;
	uint32_t rx_data_offset;
	uint32_t tx_data_offset;
	uint32_t gate_command_word_addr;
	uint32_t gate_ack_word_addr;
	uint8_t next_color;
	uint8_t nr_cis;
	uint8_t ci_mask;
	uint8_t reserved0;
};

struct pimnic_runtime_config {
	uint32_t magic;
	uint32_t version;
	uint32_t mode;
	uint32_t nr_ranks;
	uint32_t nr_groups;
	uint32_t payload_bytes;
	uint32_t messages_per_group;
	uint32_t batch_size;
	uint32_t poll_interval_us;
	uint32_t timeout_us;
	uint32_t active_group_mask;
	uint32_t nic_slowdown_us;
	uint32_t flags;
	struct pimnic_rank_config ranks[PIMNIC_MAX_RANKS];
};

struct pimnic_runtime_result {
	uint32_t magic;
	uint32_t version;
	uint32_t status;
	uint32_t nr_groups;
	uint64_t messages_injected;
	uint64_t messages_echoed;
	uint64_t pattern_errors;
	uint64_t order_errors;
	uint64_t dma_reads;
	uint64_t dma_writes;
	uint64_t dma_read_bytes;
	uint64_t dma_write_bytes;
	uint64_t ci_commands;
	uint64_t decode_faults;
	uint64_t collision_faults;
	uint64_t gate_pauses;
	uint64_t group_control_commands;
	uint64_t rx_wraps;
	uint64_t tx_wraps;
	uint64_t poll_count;
	uint64_t poll_interval_p50_ns;
	uint64_t poll_interval_p99_ns;
	uint64_t rtt_p50_ns;
	uint64_t rtt_p99_ns;
	uint64_t rtt_max_ns;
	uint64_t elapsed_ns;
	uint64_t group_messages[PIMNIC_MAX_RANKS *
				PIMNIC_GROUPS_PER_RANK];
	uint64_t group_poll_count[PIMNIC_MAX_RANKS *
				  PIMNIC_GROUPS_PER_RANK];
	uint64_t group_dma_ops[PIMNIC_MAX_RANKS *
			       PIMNIC_GROUPS_PER_RANK];
	uint8_t final_next_color[PIMNIC_MAX_RANKS];
	uint8_t reserved[8];
};

#endif
