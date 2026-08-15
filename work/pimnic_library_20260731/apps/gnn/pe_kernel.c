#include <defs.h>
#include <errno.h>
#include <mram.h>
#include <stdint.h>

#include "pimnic/paradigm/pe_paradigm.h"

#define GNN_VECTOR_ELEMS 4u

__mram_noinit __attribute__((aligned(8)))
int32_t gnn_vector[GNN_VECTOR_ELEMS];
__mram_noinit __attribute__((aligned(8)))
int32_t gnn_contribution[GNN_VECTOR_ELEMS];
/* Even cycles enter gnn_collective_id (dim-0 REDUCE), odd cycles enter
 * gnn_collective_id_alt (dim-1 GATHER) when it is non-zero, alternating
 * the axis every layer. */
__host volatile uint32_t gnn_collective_id;
__host volatile uint32_t gnn_collective_id_alt;
__host volatile uint32_t gnn_cycles;

int main(void)
{
	if (me() != 0)
		return 0;
	struct pimnic_pe pe;
	pimnic_pe_init(&pe);
	for (uint32_t cycle = 0; cycle < gnn_cycles; ++cycle) {
		__dma_aligned int32_t input[GNN_VECTOR_ELEMS];
		__dma_aligned int32_t output[GNN_VECTOR_ELEMS];
		mram_read(gnn_vector, input, sizeof(input));
		for (uint32_t i = 0; i < GNN_VECTOR_ELEMS; ++i)
			output[i] = input[i] + (int32_t)cycle;
		mram_write(output, gnn_contribution, sizeof(output));
		uint32_t collective = gnn_collective_id;
		if ((cycle & 1u) != 0 && gnn_collective_id_alt != 0)
			collective = gnn_collective_id_alt;
		int rc;
		do {
			rc = pimnic_collective_enter(
				&pe, collective,
				(uint32_t)(uintptr_t)&gnn_contribution[0],
				sizeof(output));
		} while (rc == -EAGAIN);
		if (rc != 0)
			return 1;
		for (;;) {
			pimnic_cqe_t cqe;
			rc = pimnic_poll_cq(&pe, &cqe);
			if (rc < 0)
				return 0;
			if (rc == 0 || cqe.type == PIMNIC_CQ_SEND_DONE)
				continue;
			mram_read((__mram_ptr const void *)(uintptr_t)cqe.mram_off,
				  input, sizeof(input));
			mram_write(input, gnn_vector, sizeof(input));
			pimnic_recv_release(&pe);
			break;
		}
	}
	while (pimnic_pe_poll(&pe) >= 0)
		;
	return 0;
}
