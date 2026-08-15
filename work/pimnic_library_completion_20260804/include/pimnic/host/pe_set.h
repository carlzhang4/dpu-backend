#ifndef PIMNIC_HOST_PE_SET_H
#define PIMNIC_HOST_PE_SET_H

#include <stdint.h>

#include "pimnic/abi/topology.h"
#include "pimnic/host/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pimnic_ctx pimnic_ctx_t;
typedef struct pimnic_pe_set pimnic_pe_set_t;

int pimnic_init(const pimnic_init_params_t *params, pimnic_ctx_t **out);
void pimnic_fini(pimnic_ctx_t *ctx);

int pimnic_alloc_PE(pimnic_ctx_t *ctx, uint32_t nr_pes,
		    const pimnic_topology_t *topology, pimnic_pe_set_t **out);

int pimnic_pe_set_alloc(uint32_t nr_pes, const char *profile,
			pimnic_pe_set_t **out);
int pimnic_pe_set_load(pimnic_pe_set_t *set, const char *dpu_binary);
int pimnic_pe_set_config_u32(pimnic_pe_set_t *set, const char *symbol,
			     const uint32_t *per_pe_values);
int pimnic_pe_set_boot(pimnic_pe_set_t *set);
int pimnic_pe_set_stop(pimnic_pe_set_t *set, uint32_t timeout_us);
void pimnic_pe_set_free(pimnic_pe_set_t *set);

uint32_t pimnic_pe_set_id(const pimnic_pe_set_t *set);
uint32_t pimnic_pe_set_size(const pimnic_pe_set_t *set);
const pimnic_topology_t *pimnic_pe_set_topology(const pimnic_pe_set_t *set);

#ifdef __cplusplus
}
#endif

#endif
