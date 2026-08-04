#include <assert.h>
#include <stdint.h>

#include "pimnic/abi/ring.h"
#include "pimnic/abi/topology.h"
#include "pimnic/abi/wire.h"

int main()
{
	for (uint64_t total = 0; total < 1024; ++total) {
		uint32_t offset = static_cast<uint32_t>(total & 0x7fffu);
		uint32_t length = 16u + static_cast<uint32_t>(total & 0xffu);
		uint32_t desc = pimnic_desc_pack(
			pimnic_generation_for_total(total), offset, length);
		assert(pimnic_desc_generation(desc) ==
		       pimnic_generation_for_total(total));
		assert(pimnic_desc_off64(desc) == offset);
		assert(pimnic_desc_length(desc) == length);
	}
	assert(pimnic_ring_full(0, 255));
	assert(!pimnic_ring_full(0, 254));

	uint64_t pe = pimnic_pe_pub_pack(1, 2, 3, 4, 0x5566, 7);
	assert(pimnic_pe_pub_rx_desc_head(pe) == 1);
	assert(pimnic_pe_pub_rx_data_head(pe) == 2);
	assert(pimnic_pe_pub_tx_desc_tail(pe) == 3);
	assert(pimnic_pe_pub_tx_data_tail(pe) == 4);
	assert(pimnic_pe_pub_heartbeat(pe) == 0x5566);
	assert(pimnic_pe_pub_error(pe) == 7);
	assert(pimnic_pe_pub_magic(pe) == PIMNIC_PE_PUB_MAGIC);

	uint64_t nic = pimnic_nic_pub_pack(9, 10, 0x11223344);
	assert(pimnic_nic_pub_tx_desc_head(nic) == 9);
	assert(pimnic_nic_pub_tx_data_head(nic) == 10);
	assert(pimnic_nic_pub_epoch(nic) == 0x11223344);
	return 0;
}
