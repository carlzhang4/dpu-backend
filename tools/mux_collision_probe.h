#ifndef PIMNIC_MUX_COLLISION_PROBE_H
#define PIMNIC_MUX_COLLISION_PROBE_H

#include <stdint.h>
#include <dpu_error.h>
#include <dpu_rank.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Each output byte is indexed by CI. Bit dpu_id is one when the raw mux
 * status for that (CI, dpu_id) has collision bit 7 set.
 */
dpu_error_t pimnic_read_collision_bits(struct dpu_rank_t *rank,
				       uint8_t out[8]);

#ifdef __cplusplus
}
#endif

#endif
