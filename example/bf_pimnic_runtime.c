#include <defs.h>
#include <mram.h>
#include <stdint.h>

#include "../pimnic_bf3_runtime/ring_layout.h"
#include "../pimnic_bf3_runtime/test_pattern.h"

#define CACHE_BYTES 1024u

__mram_noinit uint32_t rx_desc[PIMNIC_RX_DESC_COUNT];
__mram_noinit uint32_t tx_desc[PIMNIC_TX_DESC_COUNT];
__mram_noinit uint64_t pe_pub;
__mram_noinit uint64_t nic_pub;
__mram_noinit __attribute__((aligned(64)))
uint8_t rx_data[PIMNIC_RX_DATA_RING_BYTES];
__mram_noinit __attribute__((aligned(64)))
uint8_t tx_data[PIMNIC_TX_DATA_RING_BYTES];

__host volatile uint32_t gate_command;
__host volatile uint32_t gate_ack;
__host volatile uint32_t stop;
__host volatile uint32_t lane_id;
__host volatile uint32_t runtime_mode;
__host volatile uint32_t slowdown_cycles;
__host volatile uint32_t messages_received;
__host volatile uint32_t messages_echoed;
__host volatile uint32_t runtime_heartbeat;
__host volatile uint32_t first_error_offset;
__host volatile uint32_t error_code;

static uint32_t crc_table[256];

static void initialize_crc_table()
{
	for (uint32_t index = 0; index < 256; ++index) {
		uint32_t crc = index;
		for (uint32_t bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^
			      (0xedb88320u & (0u - (crc & 1u)));
		crc_table[index] = crc;
	}
}

static uint32_t crc32_fast(uint32_t crc, uint8_t byte)
{
	return (crc >> 8) ^ crc_table[(crc ^ byte) & 0xffu];
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

static uint8_t expected_header_byte(uint64_t seed, uint8_t lane,
				    uint32_t length, uint32_t offset)
{
	if (offset < 8)
		return (uint8_t)(seed >> (offset * 8));
	if (offset < 12)
		return (uint8_t)((uint32_t)lane >> ((offset - 8) * 8));
	return (uint8_t)(length >> ((offset - 12) * 8));
}

static uint32_t verify_and_echo(uint32_t rx_offset, uint32_t tx_offset,
				uint32_t length, uint64_t expected_seed,
				uint8_t lane,
				__dma_aligned uint8_t *cache)
{
	uint32_t crc = 0xffffffffu;
	uint32_t observed_crc = 0;
	uint32_t first_bad = UINT32_MAX;

	for (uint32_t offset = 0; offset < length; offset += CACHE_BYTES) {
		uint32_t chunk = length - offset;
		if (chunk > CACHE_BYTES)
			chunk = CACHE_BYTES;
		uint32_t dma_bytes = (chunk + 7u) & ~7u;

		mram_read(&rx_data[rx_offset + offset], cache, dma_bytes);
		for (uint32_t index = 0; index < chunk; ++index) {
			uint32_t absolute = offset + index;
			uint8_t actual = cache[index];
			if (absolute < length - PIMNIC_PATTERN_CRC_BYTES) {
				uint8_t expected =
					absolute < PIMNIC_PATTERN_HEADER_BYTES ?
						expected_header_byte(
							expected_seed, lane,
							length, absolute) :
						pimnic_pattern_body_byte(
							expected_seed, lane,
							absolute);
				if (actual != expected &&
				    first_bad == UINT32_MAX)
					first_bad = absolute;
				crc = crc32_fast(crc, actual);
			} else {
				observed_crc |=
					(uint32_t)actual
					<< ((absolute -
					     (length -
					      PIMNIC_PATTERN_CRC_BYTES)) *
					    8);
			}
		}
		mram_write(cache, &tx_data[tx_offset + offset], dma_bytes);
	}

	if ((~crc) != observed_crc && first_bad == UINT32_MAX)
		first_bad = length - PIMNIC_PATTERN_CRC_BYTES;
	return first_bad;
}

static void publish(uint8_t rx_head, uint8_t tx_tail, uint16_t heartbeat,
		    uint8_t error)
{
	__dma_aligned uint64_t value = pimnic_pe_pub_pack(
		rx_head, rx_head, tx_tail, tx_tail, heartbeat, error);
	mram_write(&value, &pe_pub, sizeof(value));
}

static void busy_slowdown(uint32_t cycles)
{
	volatile uint32_t sink = 0;
	for (uint32_t index = 0; index < cycles; ++index)
		sink += index ^ (sink << 1);
	(void)sink;
}

int main()
{
	if (me() != 0)
		return 0;

	__dma_aligned uint8_t cache[CACHE_BYTES];
	__dma_aligned uint64_t descriptor_pair = 0;
	__dma_aligned uint64_t nic_value = 0;
	uint8_t rx_head = 0;
	uint8_t tx_tail = 0;
	uint64_t rx_total = 0;
	uint64_t tx_total = 0;
	uint16_t heartbeat = 0;
	uint64_t expected_seed = 1;

	messages_received = 0;
	messages_echoed = 0;
	runtime_heartbeat = 0;
	first_error_offset = UINT32_MAX;
	error_code = 0;
	initialize_crc_table();
	gate_ack = gate_command;

	while (!stop) {
		uint32_t command = gate_command;
		if ((command & 1u) == 0) {
			gate_ack = command;
			++runtime_heartbeat;
			continue;
		}

		++heartbeat;
		++runtime_heartbeat;
		publish(rx_head, tx_tail, heartbeat, (uint8_t)error_code);

		uint32_t desc_index = rx_head;
		uint32_t pair_index = desc_index & ~1u;
		mram_read(&rx_desc[pair_index], &descriptor_pair,
			  PIMNIC_DESC_PAIR_BYTES);
		uint32_t descriptor =
			(desc_index & 1u) ?
				(uint32_t)(descriptor_pair >> 32) :
				(uint32_t)descriptor_pair;
		uint32_t expected_generation =
			pimnic_generation_for_total(rx_total);
		uint32_t length = pimnic_desc_length(descriptor);
		if (pimnic_desc_generation(descriptor) != expected_generation ||
		    length == 0)
			continue;

		uint32_t rx_offset = pimnic_desc_off64(descriptor) * 64u;
		if (length < PIMNIC_RUNTIME_PAYLOAD_MIN ||
		    length > PIMNIC_RUNTIME_PAYLOAD_MAX ||
		    rx_offset > PIMNIC_RX_DATA_RING_BYTES ||
		    length > PIMNIC_RX_DATA_RING_BYTES - rx_offset) {
			error_code = 1;
			continue;
		}

		if (runtime_mode == PIMNIC_MODE_ECHO) {
			mram_read(&nic_pub, &nic_value, sizeof(nic_value));
			uint8_t tx_head =
				pimnic_nic_pub_tx_desc_head(nic_value);
			if (pimnic_ring_full(tx_head, tx_tail))
				continue;
		}

		uint32_t first_bytes =
			length < CACHE_BYTES ? length : CACHE_BYTES;
		uint32_t first_dma = (first_bytes + 7u) & ~7u;
		mram_read(&rx_data[rx_offset], cache, first_dma);
		uint64_t seed = load_le64(cache);
		uint32_t pattern_lane = load_le32(cache + 8);
		uint32_t pattern_length = load_le32(cache + 12);
		if (seed != expected_seed) {
			error_code = 2;
			first_error_offset = 0;
		}
		if (pattern_lane != lane_id) {
			error_code = 3;
			first_error_offset = 8;
		}
		if (pattern_length != length) {
			error_code = 4;
			first_error_offset = 12;
		}

		if (runtime_mode == PIMNIC_MODE_ECHO) {
			uint32_t tx_offset =
				(uint32_t)tx_tail *
				PIMNIC_TX_DATA_UNIT_BYTES;
			uint32_t bad = verify_and_echo(
				rx_offset, tx_offset, length, expected_seed,
				(uint8_t)lane_id, cache);
			if (bad != UINT32_MAX && error_code == 0) {
				error_code = 5;
				first_error_offset = bad;
			}

			uint32_t tx_index = tx_tail;
			uint32_t tx_pair_index = tx_index & ~1u;
			mram_read(&tx_desc[tx_pair_index], &descriptor_pair,
				  PIMNIC_DESC_PAIR_BYTES);
			uint32_t tx_descriptor = pimnic_desc_pack(
				pimnic_generation_for_total(tx_total),
				tx_offset / 64u, length);
			if (tx_index & 1u)
				descriptor_pair =
					(descriptor_pair & 0xffffffffULL) |
					((uint64_t)tx_descriptor << 32);
			else
				descriptor_pair =
					(descriptor_pair &
					 0xffffffff00000000ULL) |
					tx_descriptor;
			mram_write(&descriptor_pair,
				   &tx_desc[tx_pair_index],
				   PIMNIC_DESC_PAIR_BYTES);
			tx_tail = pimnic_ring_next(tx_tail);
			++tx_total;
			++messages_echoed;
		} else {
			uint32_t crc = 0xffffffffu;
			uint32_t observed_crc = 0;
			uint32_t first_bad = UINT32_MAX;
			for (uint32_t offset = 0; offset < length;
			     offset += CACHE_BYTES) {
				uint32_t chunk = length - offset;
				if (chunk > CACHE_BYTES)
					chunk = CACHE_BYTES;
				uint32_t dma_bytes = (chunk + 7u) & ~7u;
				mram_read(&rx_data[rx_offset + offset], cache,
					  dma_bytes);
				for (uint32_t index = 0; index < chunk;
				     ++index) {
					uint32_t absolute = offset + index;
					uint8_t actual = cache[index];
					if (absolute <
					    length -
						    PIMNIC_PATTERN_CRC_BYTES) {
						uint8_t expected =
							absolute <
									PIMNIC_PATTERN_HEADER_BYTES ?
								expected_header_byte(
									expected_seed,
									(uint8_t)lane_id,
									length,
									absolute) :
								pimnic_pattern_body_byte(
									expected_seed,
									(uint8_t)lane_id,
									absolute);
						if (actual != expected &&
						    first_bad == UINT32_MAX)
							first_bad = absolute;
						crc = crc32_fast(crc,
								 actual);
					} else {
						observed_crc |=
							(uint32_t)actual
							<< ((absolute -
							     (length -
							      PIMNIC_PATTERN_CRC_BYTES)) *
							    8);
					}
				}
			}
			if (((~crc) != observed_crc ||
			     first_bad != UINT32_MAX) &&
			    error_code == 0) {
				error_code = 5;
				first_error_offset =
					first_bad == UINT32_MAX ?
						length -
							PIMNIC_PATTERN_CRC_BYTES :
						first_bad;
			}
		}

		rx_head = pimnic_ring_next(rx_head);
		++rx_total;
		++messages_received;
		++expected_seed;
		publish(rx_head, tx_tail, ++heartbeat,
			(uint8_t)error_code);
		busy_slowdown(slowdown_cycles);
	}

	return 0;
}
