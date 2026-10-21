#include "data/device_tensor_span.hpp"

#include <memory>
#include <utility>

#include "core/resource_observation.hpp"
#include "data/tensor_geometry.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::data_internal {
struct DeviceTensorSpan::Metadata {
  ResourceLease lease;
  TensorDescriptor descriptor;
  Region region;
  StridedLayout layout;
};
Result<DeviceTensorSpan> DeviceTensorSpan::from_storage(
    const TensorDescriptor& descriptor, const Region& region,
    const StridedLayout& layout,
    std::shared_ptr<const gpu_internal::DeviceStorage> storage) {
  using Answer = Result<DeviceTensorSpan>;
  if (!storage)
    return Answer(
        {ErrorCode::InvalidArgument, "missing Device tensor storage"});
  auto valid =
      validate_tensor_geometry(descriptor, region, layout, storage->size());
  if (!valid.ok())
    return Answer(valid);
  ResourceLease lease;
  if (const auto* root = resource_internal::metadata_budget()) {
    const auto bytes = descriptor.shape.size() * sizeof(std::uint64_t) +
                       region.rank() * sizeof(RegionDimension) +
                       layout.byte_strides.size() * sizeof(std::int64_t) +
                       layout.origin.size() * sizeof(std::uint64_t);
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Answer(admitted.status());
    lease = admitted.take_value();
  }
  auto metadata = std::allocate_shared<Metadata>(ResourceAllocator<Metadata>{});
  metadata->lease = std::move(lease);
  metadata->descriptor = descriptor;
  core_internal::ResourceMetadataAccess::commit(
      metadata->lease,
      metadata->descriptor.shape.capacity() * sizeof(std::uint64_t));
  metadata->region = region;
  core_internal::ResourceMetadataAccess::commit(
      metadata->lease,
      metadata->region.dimensions().capacity() * sizeof(RegionDimension));
  metadata->layout.byte_offset = layout.byte_offset;
  metadata->layout.byte_strides = layout.byte_strides;
  core_internal::ResourceMetadataAccess::commit(
      metadata->lease,
      metadata->layout.byte_strides.capacity() * sizeof(std::int64_t));
  metadata->layout.origin = layout.origin;
  core_internal::ResourceMetadataAccess::commit(
      metadata->lease,
      metadata->layout.origin.capacity() * sizeof(std::uint64_t));
  DeviceTensorSpan span;
  span.storage_ = std::move(storage);
  span.metadata_ = std::move(metadata);
  return Answer(std::move(span));
}
const Region& DeviceTensorSpan::region() const {
  return metadata_->region;
}
const StridedLayout& DeviceTensorSpan::layout() const {
  return metadata_->layout;
}
const TensorDescriptor& DeviceTensorSpan::descriptor() const {
  return metadata_->descriptor;
}
Result<DeviceTensorSpan> DeviceTensorSpan::view(const Region& wanted) const {
  if (!metadata_ || wanted.rank() != region().rank() ||
      !wanted.validate(descriptor().shape).ok())
    return Result<DeviceTensorSpan>(
        {ErrorCode::InvalidArgument, "invalid Device view"});
  for (std::size_t axis = 0; axis < wanted.rank(); ++axis) {
    const auto a = wanted.dimensions()[axis], b = region().dimensions()[axis];
    if (a.offset < b.offset || a.offset + a.extent > b.offset + b.extent)
      return Result<DeviceTensorSpan>(
          {ErrorCode::TypeMismatch, "Device view exceeds coverage"});
  }
  return from_storage(descriptor(), wanted, layout(), storage_);
}
Result<std::size_t> DeviceTensorSpan::byte_address(const std::uint64_t* at,
                                                   std::size_t rank) const {
  if (!metadata_ || !at || rank != region().rank())
    return Result<std::size_t>(
        {ErrorCode::InvalidArgument, "invalid Device coordinate"});
  for (std::size_t axis = 0; axis < rank; ++axis) {
    const auto dim = region().dimensions()[axis];
    if (at[axis] < dim.offset || at[axis] - dim.offset >= dim.extent)
      return Result<std::size_t>(
          {ErrorCode::InvalidArgument, "Device coordinate outside coverage"});
  }
  return tensor_byte_address(layout(), at, rank);
}
}  // namespace ps::data_internal
