#include <assert.h>

#include <algorithm>
#include <vector>

#include "apps/gnn/model.h"
#include "apps/kvstore/model.h"
#include "apps/select/model.h"

int main()
{
	/* Lock the kvstore hash against the published SipHash-2-4 test
	 * vectors: key 0x00..0x0F (exactly the benchmark's hash key) over
	 * the 8-byte little-endian message 00 01 .. 07 hashes to
	 * 0x93f5f5799a932462. */
	const uint64_t vector_key = 0x0706050403020100ull;
	const uint64_t vector_hash = 0x93f5f5799a932462ull;
	assert(pimnic_kvstore_index(vector_key) ==
	       vector_hash % PIMNIC_KV_TOTAL_ENTRIES);
	/* Identity table: the response is the hashed index itself, and
	 * equal keys must keep answering the same value. */
	std::vector<PimnicKvRequest> requests{{0}, {1}, {vector_key}, {1}};
	auto kv_actual = pimnic_kvstore_run(requests);
	assert(kv_actual.size() == requests.size());
	for (size_t i = 0; i < requests.size(); ++i) {
		assert(kv_actual[i].value ==
		       pimnic_kvstore_index(requests[i].key));
		assert(kv_actual[i].value < PIMNIC_KV_TOTAL_ENTRIES);
	}
	assert(kv_actual[1] == kv_actual[3]);

	/* Select keeps exactly the odd values, in row order. */
	std::vector<uint32_t> rows(64);
	for (uint32_t i = 0; i < rows.size(); ++i)
		rows[i] = i;
	auto selected = pimnic_select_run(rows);
	std::vector<uint32_t> baseline;
	std::copy_if(rows.begin(), rows.end(), std::back_inserter(baseline),
		     [](uint32_t value) { return (value & 1u) != 0; });
	assert(selected == baseline);
	assert(selected.size() == 32);
	assert(pimnic_select_run({0, 2, 4}).empty());
	assert(pimnic_select_run({}).empty());

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
