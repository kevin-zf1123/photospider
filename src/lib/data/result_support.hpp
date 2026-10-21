#pragma once

#include "photospider/data/result.hpp"

namespace ps::data_internal {
Status invalid_schema();
Status unavailable();
Status tensor_topology(const ResultTensorSpec&);

// One full-rank temporary descriptor, charged to the active execution root.
// The lease outlives the shape array, including failed construction.
struct SampleDescriptor final {
  explicit SampleDescriptor(const ResultTensorSpec& spec);
  SampleDescriptor(const SampleDescriptor&) = delete;
  SampleDescriptor& operator=(const SampleDescriptor&) = delete;
  ResourceLease metadata;
  TensorDescriptor value;
};
}  // namespace ps::data_internal
