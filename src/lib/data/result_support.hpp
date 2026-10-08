#pragma once

#include "photospider/data/result.hpp"

namespace ps::data_internal {
Status invalid_schema();
Status unavailable();
Result<std::uint64_t> scaled(const ResultExtent&, std::uint64_t);
PlanarImageLayout planar_layout(const ResultTensorLayout&);
Status tensor_topology(const ResultTensorSpec&);
}  // namespace ps::data_internal
