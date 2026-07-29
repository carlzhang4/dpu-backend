#include "mux_collision_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dpu.h>
#include <ufi/ufi.h>

#define PIMNIC_CMD_GET_MUX_CTRL 0x02u

dpu_error_t pimnic_read_collision_bits(struct dpu_rank_t *rank,
				       uint8_t out[8])
{
	if (rank == NULL || out == NULL)
		return DPU_ERR_INVALID_DPU_SET;
	memset(out, 0, 8);

	uint8_t nr_cis =
		rank->description->hw.topology.nr_of_control_interfaces;
	for (uint8_t dpu_id = 0; dpu_id < 8; ++dpu_id) {
		uint8_t ci_mask =
			nr_cis >= 8 ? 0xffu : (uint8_t)((1u << nr_cis) - 1u);
		uint8_t values[8] = { 0 };
		dpu_error_t status =
			ufi_select_dpu_even_disabled(rank, &ci_mask, dpu_id);
		if (status != DPU_OK)
			return status;
		status = ufi_write_dma_ctrl(rank, ci_mask, 0xff,
					    PIMNIC_CMD_GET_MUX_CTRL);
		if (status != DPU_OK)
			return status;
		status = ufi_clear_dma_ctrl(rank, ci_mask);
		if (status != DPU_OK)
			return status;
		status = ufi_read_dma_ctrl(rank, ci_mask, values);
		if (status != DPU_OK)
			return status;
		for (uint8_t ci = 0; ci < nr_cis; ++ci)
			if ((ci_mask & (1u << ci)) && (values[ci] & 0x80u))
				out[ci] |= (uint8_t)(1u << dpu_id);
	}
	return DPU_OK;
}

#ifdef PIMNIC_COLLISION_PROBE_MAIN
static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s [--rank N]\n", program);
}

int main(int argc, char **argv)
{
	uint32_t requested_rank = 0;
	for (int index = 1; index < argc; ++index) {
		if (!strcmp(argv[index], "--rank") && index + 1 < argc) {
			requested_rank =
				(uint32_t)strtoul(argv[++index], NULL, 0);
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (requested_rank >= 8) {
		usage(argv[0]);
		return 2;
	}

	struct dpu_set_t set;
	DPU_ASSERT(dpu_alloc((requested_rank + 1u) * 64u, "backend=hw",
			     &set));
	struct dpu_rank_t *ranks[8] = { NULL };
	uint32_t nr_ranks = 0;
	struct dpu_set_t dpu;
	DPU_FOREACH(set, dpu)
	{
		struct dpu_rank_t *rank = dpu.dpu->rank;
		uint32_t found = 0;
		for (uint32_t index = 0; index < nr_ranks; ++index)
			if (ranks[index] == rank)
				found = 1;
		if (!found)
			ranks[nr_ranks++] = rank;
	}
	if (requested_rank >= nr_ranks) {
		fprintf(stderr, "requested rank %u, allocated only %u\n",
			requested_rank, nr_ranks);
		dpu_free(set);
		return 1;
	}

	uint8_t collision[8] = { 0 };
	dpu_error_t status = pimnic_read_collision_bits(
		ranks[requested_rank], collision);
	if (status != DPU_OK) {
		fprintf(stderr, "collision read failed: %s\n",
			dpu_error_to_string(status));
		dpu_free(set);
		return 1;
	}

	uint32_t collisions = 0;
	for (uint32_t ci = 0; ci < 8; ++ci) {
		for (uint32_t dpu_id = 0; dpu_id < 8; ++dpu_id) {
			uint32_t set_bit =
				(collision[ci] >> dpu_id) & 1u;
			printf("rank=%u ci=%u dpu=%u collision=%u\n",
			       requested_rank, ci, dpu_id, set_bit);
			collisions += set_bit;
		}
	}
	printf("collision-probe %s total=%u\n",
	       collisions == 0 ? "PASS" : "COLLISION", collisions);
	DPU_ASSERT(dpu_free(set));
	return collisions == 0 ? 0 : 3;
}
#endif
