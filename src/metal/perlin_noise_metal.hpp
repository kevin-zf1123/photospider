#pragma once

#include "ps_types.hpp"

namespace ps { namespace ops {

NodeOutput op_perlin_noise_metal(const Node& node, const std::vector<const NodeOutput*>& inputs);

}}

// Export a C-callable eager-init entry so the loader can prewarm
extern "C" void perlin_noise_metal_eager_init();
