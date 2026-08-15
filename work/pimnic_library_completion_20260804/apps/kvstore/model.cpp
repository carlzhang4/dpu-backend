#include "model.h"

std::vector<PimnicKvResponse>
pimnic_kvstore_run(const std::vector<PimnicKvEntry> &table,
		   const std::vector<PimnicKvRequest> &requests)
{
	std::vector<PimnicKvResponse> output;
	output.reserve(requests.size());
	for (const auto &request : requests) {
		PimnicKvResponse response{request.key, 0, 0,
					 request.request_id};
		if (!table.empty()) {
			const auto &entry = table[request.key % table.size()];
			if (entry.valid && entry.key == request.key) {
				response.value = entry.value;
				response.found = 1;
			}
		}
		output.push_back(response);
	}
	return output;
}
