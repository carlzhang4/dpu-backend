#include "model.h"

#include "SipHash.h"

uint64_t pimnic_kvstore_index(uint64_t key)
{
	static const char hash_key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
					   0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
					   0x0C, 0x0D, 0x0E, 0x0F };
	uint64_t hash = 0;
	siphash((const char *)&key, sizeof(key), hash_key, (uint8_t *)&hash,
		sizeof(hash));
	return hash % PIMNIC_KV_TOTAL_ENTRIES;
}

std::vector<PimnicKvResponse>
pimnic_kvstore_run(const std::vector<PimnicKvRequest> &requests)
{
	std::vector<PimnicKvResponse> output;
	output.reserve(requests.size());
	for (const auto &request : requests)
		/* Identity table: value at index i is i itself. */
		output.push_back({ pimnic_kvstore_index(request.key) });
	return output;
}
