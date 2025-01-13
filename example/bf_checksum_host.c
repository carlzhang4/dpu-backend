#include <dpu.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "../pimnic/entities.h"
#include "../util/virt2phys.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./build/example/bf_checksum"
#endif

#define BUFFER_SIZE_SHIFT 10
#define BUFFER_SIZE (1 << BUFFER_SIZE_SHIFT)

//DPU_ALLOCATE_ALL
#define NUM_DPUS 1 //DPU_ALLOCATE_ALL

void copy_to(struct dpu_set_t dpu){
    uint8_t* buffer = (uint8_t*)malloc(BUFFER_SIZE);
	for (int byte_index = 0; byte_index < BUFFER_SIZE; byte_index++) {
		buffer[byte_index] = 0;
	}
	DPU_ASSERT(dpu_copy_to(dpu, "buffer", 0, buffer, BUFFER_SIZE));
}

int main() {
    struct dpu_set_t set, dpu;
	uint32_t nr_dpus = 0;
    uint32_t checksum;

    DPU_ASSERT(dpu_alloc(NUM_DPUS, NULL, &set));
    DPU_ASSERT(dpu_load(set, DPU_BINARY, NULL));

	DPU_ASSERT(dpu_get_nr_dpus(set, &checksum));
	printf("Number of DPUs = %d\n", nr_dpus);

	DPU_FOREACH(set, dpu){
		bank_init(dpu.dpu);
	}
	
	bank_list();

    printf("\nstart copy_to\n");
    DPU_FOREACH(set,dpu){
        copy_to(dpu);
    }

	export_test();
	__builtin_ia32_mfence();

	sleep(1);

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH(set, dpu){
	    DPU_ASSERT(dpu_copy_from(dpu, "checksum", 0, (uint8_t *)&checksum, sizeof(checksum)));
        printf("Computed checksum = %d\n", checksum);
    }

	printf("printing log for dpu:\n");
	DPU_FOREACH(set, dpu) {
		DPU_ASSERT(dpu_log_read(dpu, stdout));
	}

    DPU_ASSERT(dpu_free(set));

    return 0;
}