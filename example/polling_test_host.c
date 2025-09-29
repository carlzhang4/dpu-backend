#include <dpu.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "../pimnic/entities.h"
#include "../util/virt2phys.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./example/polling_test"
#endif

#define BUFFER_SIZE_SHIFT 10
#define BUFFER_SIZE (1 << BUFFER_SIZE_SHIFT)

//DPU_ALLOCATE_ALL
#define NUM_DPUS 8 //DPU_ALLOCATE_ALL

void copy_to(struct dpu_set_t dpu){
    uint8_t* buffer = (uint8_t*)malloc(BUFFER_SIZE);
	for (int byte_index = 0; byte_index < BUFFER_SIZE; byte_index++) {
		buffer[byte_index] = 0;
	}
	DPU_ASSERT(dpu_copy_to(dpu, "buffer", 0, buffer, BUFFER_SIZE));
}

void copy_value(struct dpu_set_t dpu,uint8_t value){
    uint8_t* buffer = (uint8_t*)malloc(BUFFER_SIZE);
	for (int byte_index = 0; byte_index < BUFFER_SIZE; byte_index++) {
		buffer[byte_index] = value;
	}
	DPU_ASSERT(dpu_copy_to(dpu, "buffer2", 0, buffer, BUFFER_SIZE));
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
    int kk=0;
    DPU_FOREACH(set,dpu){
        kk++;
        copy_value(dpu, kk);
    }

    bank_list();
	export_polling_test();
	__builtin_ia32_mfence();

	//sleep(10);

    // for(int i=0;i<NUM_DPUS;i++){
    //     int value;
    //     printf("Input value for DPU %d: ", i);
    //     scanf("%d", &value);
    //     bank_write_test(i, 8, value);
    // }

    uint8_t polling_flag[NUM_DPUS];

    // for(int i=0;i<NUM_DPUS;i++){
    //     polling_flag[i]=bank_read_test(i, 0);
    //     printf("Polling flag for DPU %d: %d\n", i, polling_flag[i]);
    // }

	printf("\nstart dpu kernel\n");

    DPU_ASSERT(dpu_launch(set, DPU_ASYNCHRONOUS));

    // uint32_t bytes_read = 0;
    // for(int i=1;i<=2;i++){
    //     sleep(1);
    //     for(int j=0;j<NUM_DPUS;j++){
    //         bank_write_test(i, bytes_read, i);
    //     }
    //     printf("write done for iteration %d\n", i);
    //     //* polling
    //     sleep(1);
    //     // bool all_done = false;
    //     // while(!all_done){
    //     //     all_done = true;
    //     //     for(int j=0;j<NUM_DPUS;j++){
    //     //         polling_flag[j]=bank_read_test(j, bytes_read+4*1024);
    //     //         if(polling_flag[j]!=i){
    //     //             all_done = false;
    //     //         }
    //     //     }
    //     // }
    //     for(int j=0;j<NUM_DPUS;j++){
    //         polling_flag[j]=bank_read_test(j, bytes_read+4*1024);
    //         printf("Polling flag for DPU %d: %d\n", j, polling_flag[j]);
    //     }
    //     bytes_read += (1 << 9);
    // }

    DPU_ASSERT(dpu_sync(set));

    DPU_FOREACH(set, dpu){
	    DPU_ASSERT(dpu_copy_from(dpu, "checksum", 0, (uint8_t *)&checksum, sizeof(checksum)));
        printf("Computed checksum = %d\n", checksum);
    }

    for(int i=0;i<NUM_DPUS;i++){
        for(int j=0;j<16;j++){
            polling_flag[j]=bank_read_test(i,j);
            printf("%d ", polling_flag[j]);
        }
        printf("\n");
    }

	// printf("printing log for dpu:\n");
	// DPU_FOREACH(set, dpu) {
	// 	DPU_ASSERT(dpu_log_read(dpu, stdout));
	// }

    getchar();
    DPU_ASSERT(dpu_free(set));

    return 0;
}