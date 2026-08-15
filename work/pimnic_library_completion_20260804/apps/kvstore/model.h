#ifndef PIMNIC_APPS_KVSTORE_MODEL_H
#define PIMNIC_APPS_KVSTORE_MODEL_H

#include <stdint.h>
#include <vector>

/* CPU model of the original benchmarks/kvstore GET contract
 * (kvstore_pim_multidpu.cpp + src/kvstore_get_device.c): a 32 MiB
 * identity table (key[i] = value[i] = i), SipHash-2-4 bucket index with
 * the fixed 0x00..0x0F key, and no key comparison. */
#define PIMNIC_KV_TOTAL_ENTRIES (32ull * 1024 * 1024 / 16)

struct PimnicKvRequest {
	uint64_t key;
};

struct PimnicKvResponse {
	uint64_t value;

	bool operator==(const PimnicKvResponse &other) const
	{
		return value == other.value;
	}
};

uint64_t pimnic_kvstore_index(uint64_t key);

std::vector<PimnicKvResponse>
pimnic_kvstore_run(const std::vector<PimnicKvRequest> &requests);

#endif
