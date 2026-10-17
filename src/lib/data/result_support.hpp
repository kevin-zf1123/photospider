#pragma once

#include "photospider/data/result.hpp"

namespace ps::data_internal {
Status invalid_schema();
Status unavailable();
Status tensor_topology(const ResultTensorSpec&);
}  // namespace ps::data_internal
