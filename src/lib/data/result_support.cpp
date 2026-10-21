#include "data/result_support.hpp"

#include <algorithm>
#include <new>
#include <utility>

#include "core/resource_observation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::data_internal {
SampleDescriptor::SampleDescriptor(const ResultTensorSpec& spec) {
  const auto rank = spec.batch_axes.size() + spec.descriptor.shape.size();
  if (const auto* root = resource_internal::metadata_budget()) {
    const auto bytes = rank * sizeof(std::uint64_t);
    auto admission = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admission.ok())
      throw std::bad_alloc();
    metadata = admission.take_value();
  }
  value.shape.reserve(rank);
  core_internal::ResourceMetadataAccess::commit(
      metadata, value.shape.capacity() * sizeof(std::uint64_t));
  value.shape.insert(value.shape.end(), spec.batch_axes.begin(),
                     spec.batch_axes.end());
  value.shape.insert(value.shape.end(), spec.descriptor.shape.begin(),
                     spec.descriptor.shape.end());
  value.element_type = spec.descriptor.element_type;
}
Status invalid_schema() {
  return Status{ErrorCode::TypeMismatch, "invalid structured result schema"};
}
Status unavailable() {
  return Status{ErrorCode::NotFound, "structured result range is incomplete"};
}
Status tensor_topology(const ResultTensorSpec& spec) {
  const auto rank = spec.descriptor.shape.size();
  if (!rank || !core_internal::can_add(rank, spec.batch_axes.size(), 8) ||
      spec.atomic_trailing_axes > rank ||
      std::any_of(spec.descriptor.shape.begin(), spec.descriptor.shape.end(),
                  [](auto n) { return !n; }) ||
      std::any_of(spec.batch_axes.begin(), spec.batch_axes.end(),
                  [](auto n) { return !n; }))
    return invalid_schema();
  const auto dtype = static_cast<std::uint32_t>(spec.descriptor.element_type);
  if (dtype < 1 || dtype > 7)
    return invalid_schema();
  if (spec.layout.spatial) {
    const auto& layout = spec.layout;
    const auto free_axes = rank - spec.atomic_trailing_axes;
    if (rank < 2 || layout.height_axis >= free_axes ||
        layout.width_axis >= free_axes ||
        layout.height_axis == layout.width_axis ||
        (layout.channel_axis && (*layout.channel_axis >= rank ||
                                 *layout.channel_axis == layout.height_axis ||
                                 *layout.channel_axis == layout.width_axis)))
      return invalid_schema();
  }
  return Status::success();
}
}  // namespace ps::data_internal
