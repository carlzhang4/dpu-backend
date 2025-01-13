#ifndef __ENTITIES_HPP__
#define __ENTITIES_HPP__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <dpu_rank.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "../ufi/include/ufi/ufi_config.h"

int bank_init(struct dpu_t* dpu_o);
// int bank_init(uint64_t base_region_addr, int slice_id, int dpu_id);
void bank_list();
void bank_write_test(int dpu_idx, int size_shift, int value);
void export_test();

#ifdef __cplusplus
}
#endif

#endif