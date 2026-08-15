#ifndef PIMNIC_ABI_VERSION_H
#define PIMNIC_ABI_VERSION_H

#include <stdint.h>

#define PIMNIC_ABI_MAGIC 0x50494d43u /* "PIMC" */
#define PIMNIC_ABI_VERSION 4u

#if defined(__cplusplus)
#define PIMNIC_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define PIMNIC_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#endif
