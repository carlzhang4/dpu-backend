#ifndef PIMNIC_BF3_MRAM_GUARD_H
#define PIMNIC_BF3_MRAM_GUARD_H

#include <stdint.h>
#include <dpu_error.h>
#include <dpu_rank.h>

dpu_error_t pimnic_pairline_dma_begin(struct dpu_rank_t *rank,
				      uint32_t pairline_id);
dpu_error_t pimnic_pairline_dma_end(struct dpu_rank_t *rank,
				    uint32_t pairline_id);
dpu_error_t pimnic_group_external_mram_dma_begin(struct dpu_rank_t *rank,
						 uint32_t group_in_rank);
dpu_error_t pimnic_group_external_mram_dma_end(struct dpu_rank_t *rank,
					       uint32_t group_in_rank);

#endif
