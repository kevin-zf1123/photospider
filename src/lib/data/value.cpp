#include "photospider/data/value.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps {
namespace {

/** @brief Computes a checked origin-relative address without signed overflow.
 */
Result<std::size_t> address(const StridedLayout& layout,
                            const std::vector<std::uint64_t>& coordinate) {
  std::uint64_t positive = layout.byte_offset;
  std::uint64_t negative = 0;
  if (positive > INT64_MAX)
    return Result<std::size_t>(Status::failure(
        ErrorCode::InvalidArgument, "Value byte offset exceeds int64"));
  for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
    const auto origin = layout.origin.empty() ? 0 : layout.origin[axis];
    const auto coord = coordinate[axis];
    const auto stride = layout.byte_strides[axis];
    if (coord == origin || stride == 0)
      continue;
    if (stride == INT64_MIN)
      return Result<std::size_t>(Status::failure(
          ErrorCode::InvalidArgument, "Value stride uses INT64_MIN"));
    const auto distance = coord >= origin ? coord - origin : origin - coord;
    const auto magnitude =
        static_cast<std::uint64_t>(stride < 0 ? -stride : stride);
    if (distance > static_cast<std::uint64_t>(INT64_MAX) / magnitude)
      return Result<std::size_t>(Status::failure(
          ErrorCode::InvalidArgument, "Value coordinate span overflows"));
    const auto span = distance * magnitude;
    auto& sum = ((coord < origin) != (stride < 0)) ? negative : positive;
    if (span > static_cast<std::uint64_t>(INT64_MAX) - sum)
      return Result<std::size_t>(Status::failure(
          ErrorCode::InvalidArgument, "Value address sum overflows"));
    sum += span;
  }
  if (negative > positive || positive - negative > SIZE_MAX)
    return Result<std::size_t>(Status::failure(
        ErrorCode::InvalidArgument, "Value address precedes storage"));
  return Result<std::size_t>(static_cast<std::size_t>(positive - negative));
}
}  // namespace

Result<Value> Value::create(ValueDescriptor descriptor, Region region,
                            StridedLayout layout,
                            std::vector<std::uint8_t> bytes,
                            std::vector<ValueFacet> facets) {
  auto storage = std::shared_ptr<CpuStorage>(new CpuStorage());
  storage->adopted_ =
      std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
  storage->capacity_ = storage->adopted_->capacity();
  return from_storage(std::move(descriptor), std::move(region),
                      std::move(layout), std::move(storage), std::move(facets));
}

Result<Value> Value::from_storage(ValueDescriptor descriptor, Region region,
                                  StridedLayout layout,
                                  std::shared_ptr<const CpuStorage> storage,
                                  std::vector<ValueFacet> facets) {
  if (!storage || descriptor.shape.empty() || descriptor.shape.size() > 8 ||
      descriptor.shape.size() != layout.byte_strides.size() ||
      (!layout.origin.empty() &&
       layout.origin.size() != descriptor.shape.size()) ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](std::uint64_t extent) { return extent == 0; })) {
    return Result<Value>(Status::failure(
        ErrorCode::InvalidArgument, "invalid Value storage/descriptor/layout"));
  }
  auto status = region.validate(descriptor.shape);
  if (!status.ok())
    return Result<Value>(status);
  std::size_t width = 0;
  try {
    width = element_size(descriptor.element_type);
  } catch (const std::invalid_argument& error) {
    return Result<Value>(
        Status::failure(ErrorCode::InvalidArgument, error.what()));
  }
  if (!region.empty()) {
    std::vector<std::uint64_t> low, high;
    for (std::size_t axis = 0; axis < region.rank(); ++axis) {
      auto first = region.dimensions()[axis].offset;
      auto last = first + region.dimensions()[axis].extent - 1;
      if (layout.byte_strides[axis] < 0)
        std::swap(first, last);
      low.push_back(first);
      high.push_back(last);
    }
    auto minimum = address(layout, low);
    auto maximum = address(layout, high);
    if (!minimum.ok())
      return Result<Value>(minimum.status());
    if (!maximum.ok())
      return Result<Value>(maximum.status());
    if (maximum.value() < minimum.value() ||
        maximum.value() > storage->bytes().size() ||
        width > storage->bytes().size() - maximum.value()) {
      return Result<Value>(
          Status::failure(ErrorCode::InvalidArgument,
                          "Value layout addresses outside storage"));
    }
  } else if (layout.byte_offset > storage->bytes().size()) {
    return Result<Value>(Status::failure(ErrorCode::InvalidArgument,
                                         "empty Value offset exceeds storage"));
  }
  status = input_internal::canonicalize_facets(&facets);
  if (!status.ok())
    return Result<Value>(status);
  Value value;
  value.descriptor_ = std::move(descriptor);
  value.region_ = std::move(region);
  value.layout_ = std::move(layout);
  value.facets_ = std::move(facets);
  value.storage_ = std::move(storage);
  return Result<Value>(std::move(value));
}

Result<Value> Value::view(const Region& region) const {
  if (!valid() || region.rank() != region_.rank() ||
      !region.validate(descriptor_.shape).ok())
    return Result<Value>(
        Status::failure(ErrorCode::InvalidArgument, "invalid Value subview"));
  for (std::size_t axis = 0; axis < region.rank(); ++axis) {
    const auto wanted = region.dimensions()[axis];
    const auto available = region_.dimensions()[axis];
    if (wanted.offset < available.offset ||
        wanted.offset + wanted.extent > available.offset + available.extent)
      return Result<Value>(Status::failure(
          ErrorCode::TypeMismatch, "subview exceeds available coverage"));
  }
  return from_storage(descriptor_, region, layout_, storage_, facets_);
}

Result<std::size_t> Value::byte_address(
    const std::vector<std::uint64_t>& coordinate) const {
  if (!valid() || coordinate.size() != region_.rank())
    return Result<std::size_t>(Status::failure(
        ErrorCode::InvalidArgument, "invalid Value coordinate rank"));
  for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
    const auto dim = region_.dimensions()[axis];
    if (coordinate[axis] < dim.offset ||
        coordinate[axis] - dim.offset >= dim.extent)
      return Result<std::size_t>(Status::failure(
          ErrorCode::InvalidArgument, "coordinate outside Value coverage"));
  }
  return address(layout_, coordinate);
}

/**
 * @brief Implements canonical Float64 scalar construction.
 * @copydetails Value::from_float64
 */
Value Value::from_float64(double value) {
  std::vector<std::uint8_t> bytes(sizeof(double));
  std::memcpy(bytes.data(), &value, sizeof(double));
  auto result =
      create(ValueDescriptor{ElementType::Float64, {1U}}, Region::whole({1U}),
             StridedLayout{0U, {8}}, std::move(bytes));
  if (!result.ok()) {
    throw std::logic_error("internal scalar Value construction failed");
  }
  return result.take_value();
}

/**
 * @brief Implements checked Float64 scalar extraction.
 * @copydetails Value::as_float64
 */
Result<double> Value::as_float64() const {
  const bool exact_scalar_region = region_.rank() == 1U &&
                                   region_.dimensions()[0U].offset == 0U &&
                                   region_.dimensions()[0U].extent == 1U;
  if (!valid() || descriptor_.element_type != ElementType::Float64 ||
      descriptor_.shape != std::vector<std::uint64_t>{1U} ||
      !exact_scalar_region ||
      std::any_of(layout_.origin.begin(), layout_.origin.end(),
                  [](std::uint64_t origin) { return origin != 0; }) ||
      layout_.byte_strides != std::vector<std::int64_t>{8} ||
      layout_.byte_offset + sizeof(double) > bytes().size()) {
    return Result<double>(Status::failure(
        ErrorCode::TypeMismatch, "Value is not a contiguous Float64 scalar"));
  }
  double value = 0.0;
  std::memcpy(&value, bytes().data() + layout_.byte_offset, sizeof(double));
  return Result<double>(value);
}

/**
 * @brief Implements immutable descriptor access.
 * @copydetails Value::descriptor
 */
const ValueDescriptor& Value::descriptor() const {
  if (!valid()) {
    throw std::logic_error("default Value has no descriptor");
  }
  return descriptor_;
}

/**
 * @brief Implements immutable logical Region access.
 * @copydetails Value::region
 */
const Region& Value::region() const {
  if (!valid()) {
    throw std::logic_error("default Value has no Region");
  }
  return region_;
}

/**
 * @brief Implements immutable strided-layout access.
 * @copydetails Value::layout
 */
const StridedLayout& Value::layout() const {
  if (!valid()) {
    throw std::logic_error("default Value has no layout");
  }
  return layout_;
}

/**
 * @brief Implements canonical immutable facet access.
 * @copydetails Value::facets
 */
const std::vector<ValueFacet>& Value::facets() const {
  if (!valid()) {
    throw std::logic_error("default Value has no facets");
  }
  return facets_;
}

/**
 * @brief Implements immutable byte-storage access.
 * @copydetails Value::bytes
 */
ByteView Value::bytes() const {
  if (!valid()) {
    throw std::logic_error("default Value has no bytes");
  }
  return storage_->bytes();
}

std::vector<std::uint8_t> Value::copy_bytes() const {
  const auto data = bytes();
  return data.empty() ? std::vector<std::uint8_t>()
                      : std::vector<std::uint8_t>(data.begin(), data.end());
}
const std::shared_ptr<const CpuStorage>& Value::storage() const {
  if (!valid())
    throw std::logic_error("default Value has no storage");
  return storage_;
}

/**
 * @brief Implements closed element-width lookup.
 * @copydetails Value::element_size
 */
std::size_t Value::element_size(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
      return 1U;
    case ElementType::Float32:
      return 4U;
    case ElementType::Int64:
    case ElementType::Float64:
      return 8U;
  }
  throw std::invalid_argument("unknown ElementType");
}

Result<MutableValue> MutableValue::allocate(const ValueDescriptor& descriptor,
                                            const Region& region,
                                            const BufferAllocator& allocator) {
  if (!region.validate(descriptor.shape).ok() || region.empty())
    return Result<MutableValue>(Status::failure(
        ErrorCode::InvalidArgument, "invalid writable Value region"));
  ValueDescriptor packed = descriptor;
  for (std::size_t i = 0; i < region.rank(); ++i)
    packed.shape[i] = region.dimensions()[i].extent;
  auto dense = input_internal::dense_metadata(packed);
  if (!dense.ok())
    return Result<MutableValue>(dense.status());
  auto allocation = allocator.allocate(dense.value().bytes);
  if (!allocation.ok())
    return Result<MutableValue>(allocation.status());
  MutableValue writer;
  writer.descriptor_ = descriptor;
  writer.region_ = region;
  writer.layout_ = dense.value().layout;
  for (const auto dim : region.dimensions())
    writer.layout_.origin.push_back(dim.offset);
  writer.buffer_ = allocation.take_value();
  return Result<MutableValue>(std::move(writer));
}
Result<Value> MutableValue::publish(std::vector<ValueFacet> facets) && {
  return Value::from_storage(std::move(descriptor_), std::move(region_),
                             std::move(layout_), std::move(buffer_).freeze(),
                             std::move(facets));
}

}  // namespace ps
