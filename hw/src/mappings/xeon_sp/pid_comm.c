
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/time.h>

#include "dpu_region_address_translation.h"
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <dpu.h>
#include <errno.h>
#include <limits.h>
#include <numa.h>
#include <numaif.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <x86intrin.h>
#include <immintrin.h>
#include <sys/sysinfo.h>
#include <time.h>
#include "static_verbose.h"


#define BANK_OFFSET_NEXT_DATA_a2a(i) (i * 16) // For each 64bit word, you must jump 16 * 64bit (2 cache lines)
#define BANK_CHUNK_SIZE_a2a 0x20000 //128kb
#define BANK_NEXT_CHUNK_OFFSET_a2a 0x100000 //1mb
#define MRAM_SIZE_a2a 0x4000000 // 64MB


#define CACHE_LINE 64
#define CACHE_LINE2 128


static uint32_t apply_address_translation_on_mram_offset_a2a(uint32_t byte_offset)
{
    /* We have observed that, within the 26 address bits of the MRAM address, we need to apply an address translation:
     *
     * virtual[13: 0] = physical[13: 0]
     * virtual[20:14] = physical[21:15]
     * virtual[   21] = physical[   14]
     * virtual[25:22] = physical[25:22]
     *
     * This function computes the "virtual" mram address based on the given "physical" mram address.
     */

    uint32_t mask_21_to_15 = ((1 << (21 - 15 + 1)) - 1) << 15;
    uint32_t mask_21_to_14 = ((1 << (21 - 14 + 1)) - 1) << 14;
    uint32_t bits_21_to_15 = (byte_offset & mask_21_to_15) >> 15;
    uint32_t bit_14 = (byte_offset >> 14) & 1;
    uint32_t unchanged_bits = byte_offset & ~mask_21_to_14;

    return unchanged_bits | (bits_21_to_15 << 14) | (bit_14 << 21);
}

/////////////////////////////////////////////////////////

#define RNS_FLUSH_DST(iter, dst_rank_bgwise_addr)                                  \
    do                                                                             \
    {                                                                              \
        void *dst_rank_clwise_addr1 = dst_rank_bgwise_addr;                        \
        void *dst_rank_clwise_addr2 = dst_rank_bgwise_addr + (CACHE_LINE2 * iter); \
                                                                                   \
        for (int cl = 0; cl < iter; cl++)                                          \
        {                                                                          \
            __builtin_ia32_clflushopt((uint8_t *)dst_rank_clwise_addr1);           \
            __builtin_ia32_clflushopt((uint8_t *)(dst_rank_clwise_addr2));         \
            dst_rank_clwise_addr1 += CACHE_LINE2;                                  \
            dst_rank_clwise_addr2 += CACHE_LINE2;                                  \
        }                                                                          \
    } while (0)

#define RNS_FLUSH_SRC(iter, src_rank_bgwise_addr)                                      \
    do                                                                                 \
    {                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                             \
                                                                                       \
        for (int cl = 0; cl < iter; cl++)                                              \
        {                                                                              \
            __builtin_ia32_clflushopt((uint8_t *)src_rank_clwise_addr);                \
            __builtin_ia32_clflushopt((uint8_t *)(src_rank_clwise_addr + CACHE_LINE)); \
            src_rank_clwise_addr += CACHE_LINE2;                                       \
        }                                                                              \
    } while (0)

#define RNS_SRC_BG_a2a 4
#define RNS_TAR_BG_a2a 8
#define chip_rank_a2a 64




#define RNS_COPY_a2a(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr)  \
    do                                                                                      \
    {                                                                                       \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                  \
        void *dst_rank_clwise_addr1 = dst_rank_bgwise_addr;                                 \
                                                                                            \
        __m512i reg1;                                                                       \
         __m512i reg1_rot;                                                                  \
                                                                                            \
        for (int cl = 0; cl < iter; cl++)                                                   \
        {                                                                                   \
            reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                \
            reg1_rot = _mm512_rol_epi64(reg1, rotate_bit);                                  \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1_rot);                     \
        }                                                                                   \
    } while (0)


#define S_COPY_a2a(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr)    \
    do                                                                                      \
    {                                                                                       \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                  \
        void *dst_rank_clwise_addr1 = dst_rank_bgwise_addr;                                 \
                                                                                            \
        __m512i reg1;                                                                       \
                                                                                            \
        for (int cl = 0; cl < iter; cl++)                                                   \
        {                                                                                   \
            reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1);                     \
        }                                                                                   \
    } while (0)
    

#define COMM_FLUSH_a2a(iter, src_rank_bgwise_addr)                                      \
    do                                                                                  \
    {                                                                                   \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                              \
                                                                                        \
        for (int cl = 0; cl < iter; cl++)                                               \
        {                                                                               \
            __builtin_ia32_clflushopt((uint8_t *)src_rank_clwise_addr);                 \
                                                                                        \
            src_rank_clwise_addr += CACHE_LINE2;                                        \
        }                                                                               \
    } while (0)

void xeon_sp_trans_all_to_all_rg(void *base_region_addr_src, void *base_region_addr_dst, uint32_t src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length,\
                                     uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, uint32_t num_thread, uint32_t thread_id){
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;

    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;

    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024 ;
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024;
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }

    uint32_t iter_length = num_thread*thread_id;
    uint32_t remain_length;
    if(src_start_offset%64==0 && dst_start_offset%64==0){
        iter_length=byte_length/64;
        remain_length = (byte_length%64 + 7)/8;
    }
    else{
        iter_length=0;
        remain_length = byte_length/8;
    }

    uint32_t src_rotate_group_offset_256_64= (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64;
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;



    if(alltoall_comm_type==0){ //parallel to the entangled group

        /* //threaded
        src_mram_offset = src_start_offset + 1024*1024 + 8*(byte_length/num_thread)*thread_id;
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset + 8*(byte_length/num_thread)*thread_id; */
        
        for(uint32_t rns_chip_id=0; rns_chip_id<8; rns_chip_id++){
        //for(uint32_t rns_chip_id=(8/num_thread)*thread_id; rns_chip_id<(8/num_thread)*(thread_id+1); rns_chip_id++){
            for(uint32_t i=0; i<iter_length; i++){
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

                _mm_mfence();
                void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                    src_rank_addr_before_rns += (src_off_gran);
                }
                _mm_mfence();
                
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                
                if(rns_chip_id==0){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==1){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==2){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==3){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==4){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==5){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(5, 40, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==6){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==7){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(7, 56, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }

                _mm_mfence();

                void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                    src_rank_addr_after_rns += (src_off_gran);
                }
            
                void *dst_rank_addr_after_rns = dst_rank_addr+ dst_rotate_group_offset_256_64;
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                    dst_rank_addr_after_rns += (src_off_gran);
                }
                
                src_mram_offset+=64;
                dst_mram_offset+=64;
            }

            for(uint32_t i=0; i<remain_length; i++){

                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

                _mm_mfence();
                void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                _mm_mfence();

                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                //printf("rns_chip_id: %d\n", rns_chip_id);
                if(rns_chip_id==0){
                    RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==1){
                    RNS_COPY_a2a(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==2){
                    RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==3){
                    RNS_COPY_a2a(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==4){
                    RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==5){
                    RNS_COPY_a2a(5, 40, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==6){
                    RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==7){
                    RNS_COPY_a2a(7, 56, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }

                _mm_mfence();

                void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                
                //flush dst
                void *dst_rank_addr_after_rns = dst_rank_addr+ dst_rotate_group_offset_256_64;
                COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                _mm_mfence();

                src_mram_offset+=8;
                dst_mram_offset+=8;
            }
        }
        
    }
    else{ //perpendicular to the entangled group

        for(uint32_t i=0; i<iter_length; i++){
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

            void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

            //flush before RnS
            void *src_rank_addr_before_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64;
            for(int j=0; j<8; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                src_rank_addr_before_rns += (src_off_gran);
            }
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
            for(int iter_8=0; iter_8<8; iter_8++){
                S_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                src_rank_addr_iter += (src_off_gran);
                dst_rank_addr_iter += (src_off_gran);
            }

            void *src_rank_addr_after_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64;
            for(int j=0; j<8; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                src_rank_addr_after_rns += (src_off_gran);
            }
        
            void *dst_rank_addr_after_rns = dst_rank_addr+ (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
            for(int j=0; j<8; j++){
                COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                dst_rank_addr_after_rns += (src_off_gran);
            }

            src_mram_offset+=64;
            dst_mram_offset+=64;
        }
        for(uint32_t i=0; i<remain_length; i++){
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

            void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

            //flush before RnS
            void *src_rank_addr_before_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64;
            for(int j=0; j<1; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                src_rank_addr_before_rns += (src_off_gran);
            }
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
            S_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);

            void *src_rank_addr_after_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64;
            for(int j=0; j<1; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                src_rank_addr_after_rns += (src_off_gran);
            }
        
            //flush dst
            void *dst_rank_addr_after_rns = dst_rank_addr+ (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
            for(int j=0; j<1; j++){
                COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                dst_rank_addr_after_rns += (src_off_gran);
            }
            
            src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    _mm_mfence();
    return;
}