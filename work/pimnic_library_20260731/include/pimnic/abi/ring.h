#ifndef PIMNIC_ABI_RING_H
#define PIMNIC_ABI_RING_H

#include <stdint.h>

#include "pimnic/abi/version.h"

#define PIMNIC_PE_GROUP_SIZE 16u
#define PIMNIC_DESC_COUNT 256u
#define PIMNIC_RX_DESC_COUNT PIMNIC_DESC_COUNT
#define PIMNIC_TX_DESC_COUNT PIMNIC_DESC_COUNT
#define PIMNIC_DESC_ENTRY_BYTES 4u
#define PIMNIC_DESC_PAIR_BYTES 8u
#define PIMNIC_RX_DATA_RING_BYTES (1024u * 1024u)
#define PIMNIC_TX_DATA_RING_BYTES (1024u * 1024u)
#define PIMNIC_RX_DATA_UNIT_BYTES \
	(PIMNIC_RX_DATA_RING_BYTES / PIMNIC_DESC_COUNT)
#define PIMNIC_TX_DATA_UNIT_BYTES \
	(PIMNIC_TX_DATA_RING_BYTES / PIMNIC_DESC_COUNT)

#define PIMNIC_PAYLOAD_MIN 16u
#define PIMNIC_PAYLOAD_MAX 4096u
#define PIMNIC_DESC_GEN_SHIFT 31u
#define PIMNIC_DESC_OFF64_SHIFT 16u
#define PIMNIC_DESC_OFF64_MASK 0x7fffu
#define PIMNIC_DESC_LEN_MASK 0xffffu
#define PIMNIC_PE_PUB_MAGIC 0xa5u

#define PIMNIC_MESSAGE_MAGIC 0x504d5347u /* "PMSG" */
#define PIMNIC_MESSAGE_FLAG_COLLECTIVE 0x1u
#define PIMNIC_MESSAGE_FLAG_NOOP 0x2u
#define PIMNIC_COLLECTIVE_TAG_BIT 0x80000000u

struct pimnic_message_header {
	uint32_t magic;
	uint32_t tag;
	uint32_t length;
	uint32_t flags;
};

PIMNIC_STATIC_ASSERT(sizeof(struct pimnic_message_header) == 16,
		     "pimnic_message_header ABI size");

static inline uint32_t pimnic_desc_pack(uint32_t generation,
					uint32_t off64,
					uint32_t length)
{
	return ((generation & 1u) << PIMNIC_DESC_GEN_SHIFT) |
	       ((off64 & PIMNIC_DESC_OFF64_MASK) << PIMNIC_DESC_OFF64_SHIFT) |
	       (length & PIMNIC_DESC_LEN_MASK);
}

static inline uint32_t pimnic_desc_generation(uint32_t descriptor)
{
	return descriptor >> PIMNIC_DESC_GEN_SHIFT;
}

static inline uint32_t pimnic_desc_off64(uint32_t descriptor)
{
	return (descriptor >> PIMNIC_DESC_OFF64_SHIFT) &
	       PIMNIC_DESC_OFF64_MASK;
}

static inline uint32_t pimnic_desc_length(uint32_t descriptor)
{
	return descriptor & PIMNIC_DESC_LEN_MASK;
}

static inline uint32_t pimnic_generation_for_total(uint64_t total)
{
	return 1u - (uint32_t)((total / PIMNIC_DESC_COUNT) & 1u);
}

static inline uint8_t pimnic_ring_next(uint8_t value)
{
	return (uint8_t)(value + 1u);
}

static inline int pimnic_ring_full(uint8_t head, uint8_t tail)
{
	return pimnic_ring_next(tail) == head;
}

static inline uint64_t pimnic_pe_pub_pack(uint8_t rx_desc_head,
					 uint8_t rx_data_head,
					 uint8_t tx_desc_tail,
					 uint8_t tx_data_tail,
					 uint16_t heartbeat,
					 uint8_t error_code)
{
	return (uint64_t)rx_desc_head |
	       ((uint64_t)rx_data_head << 8) |
	       ((uint64_t)tx_desc_tail << 16) |
	       ((uint64_t)tx_data_tail << 24) |
	       ((uint64_t)heartbeat << 32) |
	       ((uint64_t)error_code << 48) |
	       ((uint64_t)PIMNIC_PE_PUB_MAGIC << 56);
}

static inline uint8_t pimnic_pe_pub_rx_desc_head(uint64_t value)
{
	return (uint8_t)value;
}

static inline uint8_t pimnic_pe_pub_rx_data_head(uint64_t value)
{
	return (uint8_t)(value >> 8);
}

static inline uint8_t pimnic_pe_pub_tx_desc_tail(uint64_t value)
{
	return (uint8_t)(value >> 16);
}

static inline uint8_t pimnic_pe_pub_tx_data_tail(uint64_t value)
{
	return (uint8_t)(value >> 24);
}

static inline uint16_t pimnic_pe_pub_heartbeat(uint64_t value)
{
	return (uint16_t)(value >> 32);
}

static inline uint8_t pimnic_pe_pub_error(uint64_t value)
{
	return (uint8_t)(value >> 48);
}

static inline uint8_t pimnic_pe_pub_magic(uint64_t value)
{
	return (uint8_t)(value >> 56);
}

static inline uint64_t pimnic_nic_pub_pack(uint8_t tx_desc_head,
					  uint8_t tx_data_head,
					  uint32_t epoch)
{
	return (uint64_t)tx_desc_head |
	       ((uint64_t)tx_data_head << 8) |
	       ((uint64_t)epoch << 32);
}

static inline uint8_t pimnic_nic_pub_tx_desc_head(uint64_t value)
{
	return (uint8_t)value;
}

static inline uint8_t pimnic_nic_pub_tx_data_head(uint64_t value)
{
	return (uint8_t)(value >> 8);
}

static inline uint32_t pimnic_nic_pub_epoch(uint64_t value)
{
	return (uint32_t)(value >> 32);
}

#if (PIMNIC_DESC_COUNT != 256u)
#error "The one-byte ring protocol requires exactly 256 descriptors"
#endif
#if ((PIMNIC_RX_DATA_RING_BYTES % PIMNIC_DESC_COUNT) != 0)
#error "RX data ring must divide into 256 pointer units"
#endif
#if ((PIMNIC_TX_DATA_RING_BYTES % PIMNIC_DESC_COUNT) != 0)
#error "TX data ring must divide into 256 pointer units"
#endif
#if ((PIMNIC_RX_DATA_UNIT_BYTES % 64u) != 0)
#error "RX pointer unit must be 64-byte aligned"
#endif
#if ((PIMNIC_TX_DATA_UNIT_BYTES % 64u) != 0)
#error "TX pointer unit must be 64-byte aligned"
#endif

#endif
