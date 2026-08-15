#ifndef PIMNIC_HOST_OPS_UPMEM_H
#define PIMNIC_HOST_OPS_UPMEM_H

#include <stdint.h>

#include "pimnic/host/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pimnic_upmem_params {
	const char *device_name;
	int numa_node;
	int port;
} pimnic_upmem_params_t;

/* Production UPMEM/devx platform adapter.  The user pointer passed to
 * pimnic_init must point to a live pimnic_upmem_params_t. */
const struct pimnic_host_ops *pimnic_upmem_host_ops(void);

#ifdef __cplusplus
}
#endif

#endif
