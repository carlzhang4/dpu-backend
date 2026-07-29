#include "test_pattern.h"

#include <string.h>

static void store_le32(uint8_t *buffer, uint32_t value)
{
	for (uint32_t byte = 0; byte < 4; ++byte)
		buffer[byte] = (uint8_t)(value >> (byte * 8));
}

static void store_le64(uint8_t *buffer, uint64_t value)
{
	for (uint32_t byte = 0; byte < 8; ++byte)
		buffer[byte] = (uint8_t)(value >> (byte * 8));
}

static uint32_t load_le32(const uint8_t *buffer)
{
	uint32_t value = 0;
	for (uint32_t byte = 0; byte < 4; ++byte)
		value |= (uint32_t)buffer[byte] << (byte * 8);
	return value;
}

static uint64_t load_le64(const uint8_t *buffer)
{
	uint64_t value = 0;
	for (uint32_t byte = 0; byte < 8; ++byte)
		value |= (uint64_t)buffer[byte] << (byte * 8);
	return value;
}

void pimnic_fill_pattern(uint8_t *buffer, size_t length, uint64_t seed,
			 uint8_t lane)
{
	if (length < PIMNIC_PATTERN_HEADER_BYTES + PIMNIC_PATTERN_CRC_BYTES)
		return;

	store_le64(buffer, seed);
	store_le32(buffer + 8, lane);
	store_le32(buffer + 12, (uint32_t)length);
	for (size_t offset = PIMNIC_PATTERN_HEADER_BYTES;
	     offset < length - PIMNIC_PATTERN_CRC_BYTES; ++offset)
		buffer[offset] =
			pimnic_pattern_body_byte(seed, lane, (uint32_t)offset);

	uint32_t crc = 0xffffffffu;
	for (size_t offset = 0; offset < length - PIMNIC_PATTERN_CRC_BYTES;
	     ++offset)
		crc = pimnic_crc32_update(crc, buffer[offset]);
	store_le32(buffer + length - PIMNIC_PATTERN_CRC_BYTES, ~crc);
}

int64_t pimnic_verify_pattern(const uint8_t *buffer, size_t length,
			      uint64_t seed, uint8_t lane,
			      uint8_t *expected, uint8_t *actual)
{
	if (length < PIMNIC_PATTERN_HEADER_BYTES + PIMNIC_PATTERN_CRC_BYTES)
		return 0;

	uint8_t header[PIMNIC_PATTERN_HEADER_BYTES];
	store_le64(header, seed);
	store_le32(header + 8, lane);
	store_le32(header + 12, (uint32_t)length);
	for (size_t offset = 0; offset < PIMNIC_PATTERN_HEADER_BYTES;
	     ++offset) {
		if (buffer[offset] != header[offset]) {
			if (expected)
				*expected = header[offset];
			if (actual)
				*actual = buffer[offset];
			return (int64_t)offset;
		}
	}

	for (size_t offset = PIMNIC_PATTERN_HEADER_BYTES;
	     offset < length - PIMNIC_PATTERN_CRC_BYTES; ++offset) {
		uint8_t want =
			pimnic_pattern_body_byte(seed, lane, (uint32_t)offset);
		if (buffer[offset] != want) {
			if (expected)
				*expected = want;
			if (actual)
				*actual = buffer[offset];
			return (int64_t)offset;
		}
	}

	uint32_t crc = 0xffffffffu;
	for (size_t offset = 0; offset < length - PIMNIC_PATTERN_CRC_BYTES;
	     ++offset)
		crc = pimnic_crc32_update(crc, buffer[offset]);
	uint32_t want_crc = ~crc;
	uint32_t got_crc =
		load_le32(buffer + length - PIMNIC_PATTERN_CRC_BYTES);
	if (want_crc != got_crc) {
		for (uint32_t byte = 0; byte < 4; ++byte) {
			uint8_t want = (uint8_t)(want_crc >> (byte * 8));
			uint8_t got = buffer[length -
					     PIMNIC_PATTERN_CRC_BYTES + byte];
			if (want != got) {
				if (expected)
					*expected = want;
				if (actual)
					*actual = got;
				return (int64_t)(length -
						 PIMNIC_PATTERN_CRC_BYTES +
						 byte);
			}
		}
	}
	return -1;
}

uint64_t pimnic_pattern_seed(const uint8_t *buffer)
{
	return load_le64(buffer);
}

uint32_t pimnic_pattern_lane(const uint8_t *buffer)
{
	return load_le32(buffer + 8);
}

uint32_t pimnic_pattern_length(const uint8_t *buffer)
{
	return load_le32(buffer + 12);
}
