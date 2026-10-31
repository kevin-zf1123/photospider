// Copyright 2026 Photospider contributors
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "01-numeric/vector_lut.hpp"
#include "03-generation/runtime.hpp"

namespace ps::plugin_internal::grade::gradient_channels {
struct Configuration final {
  unsigned source_axis, columns;
  std::uint64_t query_channel, length, max_knots = 0;
  ElementType type, input_type;
  std::vector<std::uint64_t> input_shape, sample_shape, knots;
  std::vector<bool> cubic;
  std::vector<std::string> methods, curve_edges;
  std::vector<fast_curve::Edge> edges;
  fast_curve::Edge lookup_edge;
  std::string lookup_policy;
  std::optional<ResourceIdentity> profile;
  std::vector<OperationMetadata> outputs;
  Configuration(const std::vector<OperationMetadata>& inputs,
                const generation::Parameters& parameters);
};
std::vector<OperationParameterSpec> parameters();
Status validate_profile(const Configuration& config,
                        const ResourceSet& resources);
OperationDefinition compiled_operation();
OperationDefinition combined_operation();
}  // namespace ps::plugin_internal::grade::gradient_channels
