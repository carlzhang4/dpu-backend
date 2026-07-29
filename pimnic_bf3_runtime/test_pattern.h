#ifndef PIMNIC_TEST_PATTERN_H
#define PIMNIC_TEST_PATTERN_H

#include <stddef.h>
#include <stdint.h>

#define PIMNIC_PATTERN_HEADER_BYTES 16u
#define PIMNIC_PATTERN_CRC_BYTES 4u

static inline uint8_t pimnic_pattern_body_byte(uint64_t seed, uint8_t lane,
					       uint32_t offset)
{
	uint32_t value = (uint32_t)seed ^ (uint32_t)(seed >> 32) ^
			 ((uint32_t)lane << 24) ^ offset;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	return (uint8_t)(value ^ (value >> 8) ^ (value >> 16) ^
			 (value >> 24));
}

static inline uint32_t pimnic_crc32_update(uint32_t crc, uint8_t byte)
{
	crc ^= byte;
	for (uint32_t bit = 0; bit < 8; ++bit)
		crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	return crc;
}

#ifdef __cplusplus
extern "C" {
#endif

void pimnic_fill_pattern(uint8_t *buffer, size_t length, uint64_t seed,
			 uint8_t lane);
int64_t pimnic_verify_pattern(const uint8_t *buffer, size_t length,
			      uint64_t seed, uint8_t lane,
			      uint8_t *expected, uint8_t *actual);
uint64_t pimnic_pattern_seed(const uint8_t *buffer);
uint32_t pimnic_pattern_lane(const uint8_t *buffer);
uint32_t pimnic_pattern_length(const uint8_t *buffer);

#ifdef __cplusplus
}
#endif

#endif
