#include <string>
#include <map>
#include <immintrin.h>
#include <x86intrin.h>
#include <unistd.h>
#include "../ufi/include/ufi/ufi_config.h"
#include "utils.h"
#include "virt2phys.h"


void flush_cache_line(void* ptr){
    __asm__ __volatile__(
        "clflush (%0)"
        :
        : "r"(ptr)
    );
}

/* Transpose the entire 8 byte * 8 matrix*/
void byte_interleave_avx512(uint64_t *input, uint64_t *output, bool use_stream){
    __m512i mask;

    mask = _mm512_set_epi64(0x0f0b07030e0a0602ULL,
        0x0d0905010c080400ULL,

        0x0f0b07030e0a0602ULL,
        0x0d0905010c080400ULL,

        0x0f0b07030e0a0602ULL,
        0x0d0905010c080400ULL,

        0x0f0b07030e0a0602ULL,
        0x0d0905010c080400ULL);

    __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);
    __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);

    __m512i load = _mm512_i32gather_epi32(vindex, input, 1);
    __m512i transpose = _mm512_shuffle_epi8(load, mask);
    __m512i final = _mm512_permutexvar_epi32(perm, transpose);

    if (use_stream) {
        _mm512_stream_si512((void *)output, final);
        return;
    }

    _mm512_storeu_si512((void *)output, final);
}

char* bytes2string(void* addr, int bytes){
	char *res = (char *)malloc(sizeof(char) * (bytes*4 + 1));
	unsigned char *byte_addr = (unsigned char *)addr;
	int offset=0;
	for(int i=0;i<bytes;i++){
		if((i+1)%8==0){
			sprintf(res + offset, "%02X", byte_addr[i]);
			offset+=2;
			sprintf(res + offset, "\n");
			offset+=1;
		}else{
			sprintf(res + offset, "%02X ", byte_addr[i]);
			offset+=3;
		}

		if((i+1)%64==0){
			sprintf(res + offset, "\n");
			offset+=1;
		}
	}
	return res;
}
