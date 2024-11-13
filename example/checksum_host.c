#include <dpu.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#ifndef DPU_BINARY
#define DPU_BINARY "./build/example/checksum"
#endif

/* Size of the buffer for which we compute the checksum: 64KBytes. */
#define BUFFER_SIZE (1 << 6)

void populate_mram(struct dpu_set_t set) {
    uint8_t buffer[BUFFER_SIZE];
    for (int byte_index = 0; byte_index < BUFFER_SIZE; byte_index++) {
        buffer[byte_index] = (uint8_t)byte_index;
    }
    DPU_ASSERT(dpu_broadcast_to(set, "buffer", 0, buffer, BUFFER_SIZE, DPU_XFER_DEFAULT));
}

void copy_to(struct dpu_set_t dpu){
	uint8_t buffer[BUFFER_SIZE];
	for (int byte_index = 0; byte_index < BUFFER_SIZE; byte_index++) {
		buffer[byte_index] = byte_index;
	}
	DPU_ASSERT(dpu_copy_to(dpu, "buffer", 0, buffer, sizeof(buffer)));
}

void copy_from(struct dpu_set_t dpu){
	uint8_t buffer[BUFFER_SIZE];
	for (int byte_index = 0; byte_index < BUFFER_SIZE; byte_index++) {
		buffer[byte_index] = 1;
	}
	DPU_ASSERT(dpu_copy_from(dpu, "buffer", 0, buffer, sizeof(buffer)));
}

int main() {
    struct dpu_set_t set, dpu;
    uint32_t checksum;

    DPU_ASSERT(dpu_alloc(1, NULL, &set));
    DPU_ASSERT(dpu_load(set, DPU_BINARY, NULL));

    printf("\nstart copy_to\n");
    DPU_FOREACH(set,dpu){
        copy_to(dpu);
    }
    printf("\nstart copy_from\n");

	sleep(3);
    // DPU_FOREACH(set,dpu){
    //     copy_from(dpu);
    // }
    // printf("End broadcast\n\n");

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH(set, dpu){
	    DPU_ASSERT(dpu_copy_from(dpu, "checksum", 0, (uint8_t *)&checksum, sizeof(checksum)));
        printf("Computed checksum = %d\n", checksum);
    }

    DPU_ASSERT(dpu_free(set));
    return 0;
}