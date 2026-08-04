#include <assert.h>

#include <algorithm>
#include <vector>

#include "apps/gnn/model.h"
#include "apps/kvstore/model.h"
#include "apps/select/model.h"

int main()
{
	std::vector<PimnicKvEntry> table(16);
	for (uint32_t key = 0; key < table.size(); ++key)
		table[key] = PimnicKvEntry{key, key * 100u + 7u, true};
	std::vector<PimnicKvRequest> requests{
		{0, 10}, {3, 11}, {15, 12}, {99, 13}};
	auto kv_actual = pimnic_kvstore_run(table, requests);
	std::vector<PimnicKvResponse> kv_expected;
	for (const auto &request : requests) {
		PimnicKvResponse response{request.key, 0, 0,
					 request.request_id};
		for (const auto &entry : table)
			if (entry.valid && entry.key == request.key) {
				response.value = entry.value;
				response.found = 1;
				break;
			}
		kv_expected.push_back(response);
	}
	assert(kv_actual == kv_expected);

	std::vector<uint32_t> rows(64);
	for (uint32_t i = 0; i < rows.size(); ++i)
		rows[i] = i;
	auto none = pimnic_select_run(rows, 100, 200);
	assert(none.count == 0);
	auto capped = pimnic_select_run(rows, 0, 63);
	assert(capped.count == PIMNIC_SELECT_MAX_MATCHES);
	std::vector<uint32_t> baseline;
	std::copy_if(rows.begin(), rows.end(), std::back_inserter(baseline),
		     [](uint32_t value) {
			     return value >= 13 && value <= 27;
		     });
	auto selected = pimnic_select_run(rows, 13, 27);
	assert(selected.count == baseline.size());
	for (uint32_t i = 0; i < selected.count; ++i)
		assert(selected.values[i] == baseline[i]);

	std::vector<int32_t> input{-7, 0, 12, 44};
	for (uint32_t cycle = 0; cycle < 8; ++cycle) {
		auto actual = pimnic_gnn_layer(input, cycle);
		std::vector<int32_t> expected(input.size());
		std::transform(
			input.begin(), input.end(), expected.begin(),
			[cycle](int32_t value) {
				return value + static_cast<int32_t>(cycle);
			});
		assert(actual == expected);
	}
	return 0;
}
