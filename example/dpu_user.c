#include <mram.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <barrier.h>
#include <alloc.h>
#include <seqread.h>
#include <stdlib.h>

#define Data_size 8
#define Nr_dpus 64
#define Nr_tasklets 16
#define BYTE 8

BARRIER_INIT(tasklet_8_barrier, Nr_tasklets);

int main(){
    uint32_t tasklet_id = me();
    if(tasklet_id==0){
        mem_reset(); //reset heap memory
    }
    barrier_wait(&tasklet_8_barrier);

    printf("\n");
    barrier_wait(&tasklet_8_barrier);

    return 0;
}