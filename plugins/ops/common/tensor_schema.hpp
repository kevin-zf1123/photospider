#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "photospider/data/result.hpp"

namespace ps::plugin_internal::numeric_ops {
inline SchemaTemplate numeric_tensor_schema(
    ElementType dtype, const std::vector<uint64_t>& shape) {
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {dtype, shape};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
}  // namespace ps::plugin_internal::numeric_ops
