#ifndef PIMNIC_HOST_MAILBOX_H
#define PIMNIC_HOST_MAILBOX_H

#include <stdint.h>

#include "pimnic/host/pe_set.h"

#ifdef __cplusplus
extern "C" {
#endif

int pimnic_mailbox_read_u32(pimnic_pe_set_t *set, uint32_t pe,
			    const char *symbol, uint32_t *value);
int pimnic_mailbox_write_u32(pimnic_pe_set_t *set, uint32_t pe,
			     const char *symbol, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
