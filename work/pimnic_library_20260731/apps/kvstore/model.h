#ifndef PIMNIC_APPS_KVSTORE_MODEL_H
#define PIMNIC_APPS_KVSTORE_MODEL_H

#include <stdint.h>
#include <vector>

struct PimnicKvEntry {
	uint32_t key;
	uint32_t value;
	bool valid;
};

struct PimnicKvRequest {
	uint32_t key;
	uint32_t request_id;
};

struct PimnicKvResponse {
	uint32_t key;
	uint32_t value;
	uint32_t found;
	uint32_t request_id;

	bool operator==(const PimnicKvResponse &other) const
	{
		return key == other.key && value == other.value &&
		       found == other.found && request_id == other.request_id;
	}
};

std::vector<PimnicKvResponse>
pimnic_kvstore_run(const std::vector<PimnicKvEntry> &table,
		   const std::vector<PimnicKvRequest> &requests);

#endif
