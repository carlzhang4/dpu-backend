#include "mram_guard.h"

extern "C" {
#include "../ufi/include/ufi/ufi_config.h"
}

dpu_error_t pimnic_pairline_dma_begin(struct dpu_rank_t *rank,
				      uint32_t pairline_id)
{
	if (rank == nullptr || pairline_id >= 4)
		return DPU_ERR_INVALID_DPU_SET;
	return fifo_dpu_switch_mux_for_dpu_line(
		rank, (uint8_t)(pairline_id * 2u), 0xff);
}

dpu_error_t pimnic_pairline_dma_end(struct dpu_rank_t *rank,
				    uint32_t pairline_id)
{
	if (rank == nullptr || pairline_id >= 4)
		return DPU_ERR_INVALID_DPU_SET;
	return release_fifo_dpu_switch_mux_for_dpu_line(
		rank, (uint8_t)(pairline_id * 2u), 0xff);
}

dpu_error_t pimnic_group_external_mram_dma_begin(struct dpu_rank_t *rank,
						 uint32_t group_in_rank)
{
	if (group_in_rank >= 4)
		return DPU_ERR_INVALID_DPU_SET;
	uint32_t lower_pairline = group_in_rank / 2u;
	uint32_t upper_pairline = lower_pairline + 2u;
	dpu_error_t status =
		pimnic_pairline_dma_begin(rank, lower_pairline);
	if (status != DPU_OK)
		return status;
	status = pimnic_pairline_dma_begin(rank, upper_pairline);
	if (status != DPU_OK)
		pimnic_pairline_dma_end(rank, lower_pairline);
	return status;
}

dpu_error_t pimnic_group_external_mram_dma_end(struct dpu_rank_t *rank,
					       uint32_t group_in_rank)
{
	if (group_in_rank >= 4)
		return DPU_ERR_INVALID_DPU_SET;
	uint32_t lower_pairline = group_in_rank / 2u;
	uint32_t upper_pairline = lower_pairline + 2u;
	dpu_error_t upper =
		pimnic_pairline_dma_end(rank, upper_pairline);
	dpu_error_t lower =
		pimnic_pairline_dma_end(rank, lower_pairline);
	return upper != DPU_OK ? upper : lower;
}
