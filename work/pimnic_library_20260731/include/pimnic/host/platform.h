#ifndef PIMNIC_HOST_PLATFORM_H
#define PIMNIC_HOST_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "pimnic/abi/topology.h"
#include "pimnic/abi/wire.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pimnic_host_ops {
	int (*alloc)(void *user, uint32_t nr_pes, const char *profile,
		     void **platform_set);
	int (*load)(void *user, void *platform_set, const char *dpu_binary);
	int (*config_u32)(void *user, void *platform_set, const char *symbol,
			  const uint32_t *per_pe_values, uint32_t nr_pes);
	int (*boot)(void *user, void *platform_set);
	int (*stop)(void *user, void *platform_set, uint32_t timeout_us);
	void (*free_set)(void *user, void *platform_set);
	int (*preload)(void *user, void *platform_set, const char *symbol,
		       uint32_t offset, const void *host_src,
		       uint32_t bytes_per_pe, const pimnic_dim_op_t *dims,
		       uint32_t nr_dims);
	int (*collective_define)(void *user, void *platform_set,
				 const pimnic_collective_spec_t *spec,
				 uint32_t collective_id);
	int (*mailbox_read_u32)(void *user, void *platform_set, uint32_t pe,
				const char *symbol, uint32_t *value);
	int (*mailbox_write_u32)(void *user, void *platform_set, uint32_t pe,
				 const char *symbol, uint32_t value);
	int (*export_open)(void *user, void *platform_set, const void *params,
			   void **platform_exporter, int *control_fd);
	void (*export_close)(void *user, void *platform_exporter);
	int (*handoff_build_config)(
		void *user, void *platform_set, void *platform_exporter,
		struct pimnic_runtime_config *config);
	int (*handoff_start)(void *user, void *platform_exporter,
			     const struct pimnic_runtime_config *config);
	int (*group_set_active)(void *user, void *platform_exporter,
				uint32_t group, int active);
	int (*wait_result)(void *user, void *platform_exporter,
			   struct pimnic_runtime_result *result, int timeout_ms);
	int (*handoff_reclaim)(void *user, void *platform_set);
};

typedef struct pimnic_init_params {
	const struct pimnic_host_ops *ops;
	void *user;
	const char *profile;
} pimnic_init_params_t;

#ifdef __cplusplus
}
#endif

#endif
