#ifndef PIMNIC_HOST_EXPORT_H
#define PIMNIC_HOST_EXPORT_H

#include <stdint.h>

#include "pimnic/host/pe_set.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pimnic_exporter pimnic_exporter_t;

struct pimnic_export_params {
	const char *bind_address;
	uint16_t port;
	uint16_t reserved;
};

int pimnic_export_open(pimnic_pe_set_t *set,
		       const struct pimnic_export_params *params,
		       pimnic_exporter_t **out);
int pimnic_export_fd(const pimnic_exporter_t *exporter);
void pimnic_export_close(pimnic_exporter_t *exporter);

#ifdef __cplusplus
}
#endif

#endif
