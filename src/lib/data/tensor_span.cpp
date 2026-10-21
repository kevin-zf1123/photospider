#include "photospider/data/tensor_span.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/resource_observation.hpp"
#include "data/tensor_geometry.hpp"
#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps {
struct TensorSpan::Metadata {
  ResourceLease lease;
  TensorDescriptor descriptor;
  Region region;
  StridedLayout layout;
};
TensorSpan::~TensorSpan() noexcept {
  metadata_.reset();
}
void TensorSpan::swap(TensorSpan& other) noexcept {
  using std::swap;
  swap(metadata_, other.metadata_);
  swap(storage_, other.storage_);
}
TensorSpan& TensorSpan::operator=(const TensorSpan& other) {
  if (this != &other) {
    TensorSpan replacement(other);
    swap(replacement);
  }
  return *this;
}
TensorSpan& TensorSpan::operator=(TensorSpan&& other) noexcept {
  if (this != &other) {
    TensorSpan replacement(std::move(other));
    swap(replacement);
  }
  return *this;
}

Result<TensorSpan> TensorSpan::create(TensorDescriptor descriptor,
                                      Region region, StridedLayout layout,
                                      std::vector<std::uint8_t> bytes) {
  auto storage = std::shared_ptr<CpuStorage>(new CpuStorage());
  storage->adopted_ =
      std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
  storage->capacity_ = storage->adopted_->capacity();
  return from_storage(std::move(descriptor), std::move(region),
                      std::move(layout), std::move(storage));
}

Result<TensorSpan> TensorSpan::from_storage(
    const TensorDescriptor& descriptor, const Region& region,
    const StridedLayout& layout, std::shared_ptr<const CpuStorage> storage) {
  if (!storage)
    return Result<TensorSpan>(
        {ErrorCode::InvalidArgument, "missing tensor storage"});
  auto valid = data_internal::validate_tensor_geometry(
      descriptor, region, layout, storage->bytes().size());
  if (!valid.ok())
    return Result<TensorSpan>(valid);
  ResourceLease lease;
  if (const auto* root = resource_internal::metadata_budget()) {
    const auto bytes = descriptor.shape.size() * sizeof(std::uint64_t) +
                       region.rank() * sizeof(RegionDimension) +
                       layout.byte_strides.size() * sizeof(std::int64_t) +
                       layout.origin.size() * sizeof(std::uint64_t);
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Result<TensorSpan>(admitted.status());
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
  TensorSpan value;
  value.metadata_ = std::move(metadata);
  value.storage_ = std::move(storage);
  return Result<TensorSpan>(std::move(value));
}

Result<TensorSpan> TensorSpan::view(const Region& region) const {
  if (!valid() || region.rank() != metadata_->region.rank() ||
      !region.validate(metadata_->descriptor.shape).ok())
    return Result<TensorSpan>(Status::failure(ErrorCode::InvalidArgument,
                                              "invalid TensorSpan subview"));
  for (std::size_t axis = 0; axis < region.rank(); ++axis) {
    const auto wanted = region.dimensions()[axis];
    const auto available = metadata_->region.dimensions()[axis];
    if (wanted.offset < available.offset ||
        wanted.offset + wanted.extent > available.offset + available.extent)
      return Result<TensorSpan>(Status::failure(
          ErrorCode::TypeMismatch, "subview exceeds available coverage"));
  }
  return from_storage(metadata_->descriptor, region, metadata_->layout,
                      storage_);
}

Result<std::size_t> TensorSpan::byte_address(
    const std::vector<std::uint64_t>& coordinate) const {
  return byte_address(coordinate.data(), coordinate.size());
}

Result<std::size_t> TensorSpan::byte_address(const std::uint64_t* coordinate,
                                             std::size_t rank) const {
  if (!valid() || !coordinate || rank != metadata_->region.rank())
    return Result<std::size_t>(Status::failure(
        ErrorCode::InvalidArgument, "invalid TensorSpan coordinate rank"));
  for (std::size_t axis = 0; axis < rank; ++axis) {
    const auto dim = metadata_->region.dimensions()[axis];
    if (coordinate[axis] < dim.offset ||
        coordinate[axis] - dim.offset >= dim.extent)
      return Result<std::size_t>(
          Status::failure(ErrorCode::InvalidArgument,
                          "coordinate outside TensorSpan coverage"));
  }
  return data_internal::tensor_byte_address(metadata_->layout, coordinate,
                                            rank);
}

/**
 * @brief Implements canonical Float64 scalar construction.
 * @copydetails TensorSpan::from_float64
 */
TensorSpan TensorSpan::from_float64(double value) {
  std::vector<std::uint8_t> bytes(sizeof(double));
  std::memcpy(bytes.data(), &value, sizeof(double));
  auto result =
      create(TensorDescriptor{ElementType::Float64, {1U}}, Region::whole({1U}),
             StridedLayout{0U, {8}}, std::move(bytes));
  if (!result.ok()) {
    throw std::logic_error("internal scalar TensorSpan construction failed");
  }
  return result.take_value();
}

/**
 * @brief Implements checked Float64 scalar extraction.
 * @copydetails TensorSpan::as_float64
 */
Result<double> TensorSpan::as_float64() const {
  if (!valid())
    return Result<double>(
        Status{ErrorCode::TypeMismatch, "invalid scalar span"});
  const bool exact_scalar_region =
      metadata_->region.rank() == 1U &&
      metadata_->region.dimensions()[0U].offset == 0U &&
      metadata_->region.dimensions()[0U].extent == 1U;
  if (!valid() || metadata_->descriptor.element_type != ElementType::Float64 ||
      metadata_->descriptor.shape != std::vector<std::uint64_t>{1U} ||
      !exact_scalar_region ||
      std::any_of(metadata_->layout.origin.begin(),
                  metadata_->layout.origin.end(),
                  [](std::uint64_t origin) { return origin != 0; }) ||
      metadata_->layout.byte_strides != std::vector<std::int64_t>{8} ||
      metadata_->layout.byte_offset + sizeof(double) > bytes().size()) {
    return Result<double>(
        Status::failure(ErrorCode::TypeMismatch,
                        "TensorSpan is not a contiguous Float64 scalar"));
  }
  double value = 0.0;
  std::memcpy(&value, bytes().data() + metadata_->layout.byte_offset,
              sizeof(double));
  return Result<double>(value);
}

/**
 * @brief Implements immutable descriptor access.
 * @copydetails TensorSpan::descriptor
 */
const TensorDescriptor& TensorSpan::descriptor() const {
  if (!valid()) {
    throw std::logic_error("default TensorSpan has no descriptor");
  }
  return metadata_->descriptor;
}

/**
 * @brief Implements immutable logical Region access.
 * @copydetails TensorSpan::region
 */
const Region& TensorSpan::region() const {
  if (!valid()) {
    throw std::logic_error("default TensorSpan has no Region");
  }
  return metadata_->region;
}

/**
 * @brief Implements immutable strided-layout access.
 * @copydetails TensorSpan::layout
 */
const StridedLayout& TensorSpan::layout() const {
  if (!valid()) {
    throw std::logic_error("default TensorSpan has no layout");
  }
  return metadata_->layout;
}

/**
 * @brief Implements immutable byte-storage access.
 * @copydetails TensorSpan::bytes
 */
ByteView TensorSpan::bytes() const {
  if (!valid()) {
    throw std::logic_error("default TensorSpan has no bytes");
  }
  return storage_->bytes();
}

std::vector<std::uint8_t> TensorSpan::copy_bytes() const {
  const auto data = bytes();
  return data.empty() ? std::vector<std::uint8_t>()
                      : std::vector<std::uint8_t>(data.begin(), data.end());
}
const std::shared_ptr<const CpuStorage>& TensorSpan::storage() const {
  if (!valid())
    throw std::logic_error("default TensorSpan has no storage");
  return storage_;
}

/**
 * @brief Implements closed element-width lookup.
 * @copydetails TensorSpan::element_size
 */
std::size_t TensorSpan::element_size(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
    case ElementType::Int8:
      return 1U;
    case ElementType::UInt16:
    case ElementType::Int16:
      return 2U;
    case ElementType::Float32:
      return 4U;
    case ElementType::Int64:
    case ElementType::Float64:
      return 8U;
  }
  throw std::invalid_argument("unknown ElementType");
}

Result<TensorWriter> TensorWriter::allocate(const TensorDescriptor& descriptor,
                                            const Region& region,
                                            const BufferAllocator& allocator) {
  if (!region.validate(descriptor.shape).ok() || region.empty())
    return Result<TensorWriter>(Status::failure(
        ErrorCode::InvalidArgument, "invalid writable TensorSpan region"));
  TensorDescriptor packed = descriptor;
  for (std::size_t i = 0; i < region.rank(); ++i)
    packed.shape[i] = region.dimensions()[i].extent;
  auto dense = input_internal::dense_metadata(packed);
  if (!dense.ok())
    return Result<TensorWriter>(dense.status());
  auto allocation = allocator.allocate(dense.value().bytes);
  if (!allocation.ok())
    return Result<TensorWriter>(allocation.status());
  TensorWriter writer;
  writer.descriptor_ = descriptor;
  writer.region_ = region;
  writer.layout_ = dense.value().layout;
  for (const auto dim : region.dimensions())
    writer.layout_.origin.push_back(dim.offset);
  writer.buffer_ = allocation.take_value();
  return Result<TensorWriter>(std::move(writer));
}
Result<TensorSpan> TensorWriter::publish() && {
  return TensorSpan::from_storage(std::move(descriptor_), std::move(region_),
                                  std::move(layout_),
                                  std::move(buffer_).freeze());
}

}  // namespace ps
