#pragma once

#include "ps_types.hpp"

namespace ps { namespace ops {

NodeOutput op_perlin_noise_metal(const Node& node, const std::vector<const NodeOutput*>& inputs);

}}