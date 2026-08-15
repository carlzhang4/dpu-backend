#ifndef PIMNIC_DPU_PE_H
#define PIMNIC_DPU_PE_H

#include <stdint.h>

#include "pimnic/abi/ring.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pimnic_rx_view {
	uint32_t mram_offset;
	uint32_t length;
};

struct __attribute__((aligned(8))) pimnic_pe {
	uint8_t rx_head;
	uint8_t tx_tail;
	uint8_t error;
	uint8_t pending_rx;
	uint8_t reserved_tx;
	uint8_t recv_posted;
	uint8_t send_pending;
	uint8_t reserved0;
	uint16_t heartbeat;
	uint16_t reserved1;
	uint32_t first_error_offset;
	uint32_t pending_rx_offset;
	uint32_t pending_rx_length;
	uint32_t reserved_tx_offset;
	uint32_t reserved_tx_length;
	uint32_t recv_expected_tag;
	uint32_t recv_max_len;
	uint32_t send_tag;
	uint64_t rx_total;
	uint64_t tx_total;
};

void pimnic_pe_init(struct pimnic_pe *pe);
int pimnic_pe_poll(struct pimnic_pe *pe);
int pimnic_rx_peek(struct pimnic_pe *pe, struct pimnic_rx_view *view);
void pimnic_rx_release(struct pimnic_pe *pe);
int pimnic_tx_reserve(struct pimnic_pe *pe, uint32_t length,
		      uint32_t *tx_mram_offset);
void pimnic_tx_commit(struct pimnic_pe *pe, uint32_t length);
void pimnic_pe_set_error(struct pimnic_pe *pe, uint8_t code,
			 uint32_t offset);

#ifdef __cplusplus
}
#endif

#endif
