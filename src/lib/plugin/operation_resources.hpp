#pragma once

#include <utility>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/plugin/operation_types.hpp"

namespace ps::plugin_internal {
/** @brief Resolve only declared input/output identities before callbacks. */
inline Result<ResourceBindings> admit_operation_resources(
    const ResourceBindings& supplied,
    const std::vector<OperationMetadata>& inputs,
    const OperationMetadata& output) {
  ResourceBindings accepted;
  const auto select = [&](const std::vector<ValueFacet>& facets) {
    auto subset = supplied.select(facets);
    if (!subset.ok())
      return subset.status();
    auto joined = accepted.unite(subset.value());
    if (!joined.ok())
      return joined.status();
    accepted = joined.take_value();
    return Status::success();
  };
  const auto select_metadata = [&](const OperationMetadata& metadata) {
    auto status = select(metadata.facets);
    if (!status.ok())
      return status;
    if (metadata.result_schema) {
      auto subset = metadata.result_schema->select_resources(supplied);
      if (!subset.ok())
        return subset.status();
      auto joined = accepted.unite(subset.value());
      if (!joined.ok())
        return joined.status();
      accepted = joined.take_value();
    }
    return Status::success();
  };
  for (const auto& input : inputs) {
    auto status = select_metadata(input);
    if (!status.ok())
      return Result<ResourceBindings>(status);
  }
  auto status = select_metadata(output);
  if (!status.ok())
    return Result<ResourceBindings>(status);
  if (const auto* root = resource_internal::metadata_budget())
    return accepted.reference(*root);
  return Result<ResourceBindings>(std::move(accepted));
}
}  // namespace ps::plugin_internal
