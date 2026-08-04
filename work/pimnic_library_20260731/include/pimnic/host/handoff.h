#ifndef PIMNIC_HOST_HANDOFF_H
#define PIMNIC_HOST_HANDOFF_H

#include <stdint.h>

#include "pimnic/abi/wire.h"
#include "pimnic/host/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pimnic_session_params {
	uint32_t poll_interval_us;
	uint32_t timeout_us;
	uint32_t active_group_mask;
	uint32_t flags;
	uint8_t app_config[PIMNIC_APP_CONFIG_BYTES];
} pimnic_session_params_t;

int pimnic_handoff_build_config(
	pimnic_pe_set_t *set, pimnic_exporter_t *exporter,
	const pimnic_session_params_t *params,
	struct pimnic_runtime_config *config);
int pimnic_handoff_start(pimnic_exporter_t *exporter,
			 const struct pimnic_runtime_config *config);
int pimnic_group_set_active(pimnic_exporter_t *exporter, uint32_t group,
			    int active);
int pimnic_handoff_wait_result(pimnic_exporter_t *exporter,
			       struct pimnic_runtime_result *result,
			       int timeout_ms);
int pimnic_handoff_reclaim(pimnic_pe_set_t *set);

#ifdef __cplusplus
}
#endif

#endif
