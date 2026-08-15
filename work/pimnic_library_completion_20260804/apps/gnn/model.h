#ifndef PIMNIC_APPS_GNN_MODEL_H
#define PIMNIC_APPS_GNN_MODEL_H

#include <stdint.h>
#include <vector>

std::vector<int32_t> pimnic_gnn_layer(const std::vector<int32_t> &input,
				      uint32_t cycle);

#endif
