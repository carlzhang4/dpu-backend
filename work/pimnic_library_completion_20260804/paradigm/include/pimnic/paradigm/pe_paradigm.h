#ifndef PIMNIC_PARADIGM_PE_H
#define PIMNIC_PARADIGM_PE_H

#include <stdint.h>

#include "pimnic/dpu/pe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIMNIC_CQ_RECV 1u
#define PIMNIC_CQ_SEND_DONE 2u

typedef struct pimnic_cqe {
	uint8_t type;
	uint8_t reserved[3];
	uint32_t tag;
	uint32_t mram_off;
	uint32_t len;
} pimnic_cqe_t;

PIMNIC_STATIC_ASSERT(sizeof(pimnic_cqe_t) == 16, "pimnic_cqe ABI size");

/* UPMEM represents an __mram symbol pointer as an offset from the start of
 * MRAM.  Tag such offsets before passing them to the paradigm API so they
 * remain distinguishable from offsets into the library rx_data ring. */
#define PIMNIC_MRAM_ABSOLUTE_FLAG 0x08000000u
#define PIMNIC_MRAM_ABSOLUTE(mram_pointer) \
	(PIMNIC_MRAM_ABSOLUTE_FLAG | \
	 ((uint32_t)(uintptr_t)(mram_pointer) & ~PIMNIC_MRAM_ABSOLUTE_FLAG))
int pimnic_post_remote_send(struct pimnic_pe *pe, uint32_t tag,
			    uint32_t mram_src_off, uint32_t len);
int pimnic_post_remote_receive(struct pimnic_pe *pe, uint32_t tag,
			       uint32_t max_len);
int pimnic_poll_cq(struct pimnic_pe *pe, pimnic_cqe_t *cqe);
void pimnic_recv_release(struct pimnic_pe *pe);
int pimnic_collective_enter(struct pimnic_pe *pe, uint32_t collective_id,
			    uint32_t mram_src_off, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
