#ifndef __UTILS_HPP__
#define __UTILS_HPP__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

char* bytes2string(void* addr, int bytes);
void flush_cache_line(void* ptr);
void byte_interleave_avx512(uint64_t *input, uint64_t *output, bool use_stream);
#ifdef __cplusplus
}
#endif





#endif