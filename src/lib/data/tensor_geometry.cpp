#include "data/tensor_geometry.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "photospider/core/checked_math.hpp"

namespace ps::data_internal {

/** @brief Computes a checked origin-relative address without signed overflow.
 */
Result<std::size_t> tensor_byte_address(const StridedLayout& layout,
                                        const std::uint64_t* coordinate,
                                        std::size_t rank) {
  std::uint64_t positive = layout.byte_offset;
  std::uint64_t negative = 0;
  if (positive > INT64_MAX)
    return Result<std::size_t>(Status::failure(
        ErrorCode::InvalidArgument, "TensorSpan byte offset exceeds int64"));
  for (std::size_t axis = 0; axis < rank; ++axis) {
    const auto origin = layout.origin.empty() ? 0 : layout.origin[axis];
    const auto coord = coordinate[axis];
    const auto stride = layout.byte_strides[axis];
    if (coord == origin || stride == 0)
      continue;
    if (stride == INT64_MIN)
      return Result<std::size_t>(Status::failure(
          ErrorCode::InvalidArgument, "TensorSpan stride uses INT64_MIN"));
    const auto distance = coord >= origin ? coord - origin : origin - coord;
    const auto magnitude =
        static_cast<std::uint64_t>(stride < 0 ? -stride : stride);
    if (!core_internal::can_multiply(distance, magnitude, INT64_MAX))
      return Result<std::size_t>(Status::failure(
          ErrorCode::InvalidArgument, "TensorSpan coordinate span overflows"));
    const auto span = distance * magnitude;
    auto& sum = ((coord < origin) != (stride < 0)) ? negative : positive;
    if (!core_internal::can_add(sum, span, INT64_MAX))
      return Result<std::size_t>(Status::failure(
          ErrorCode::InvalidArgument, "TensorSpan address sum overflows"));
    sum += span;
  }
  if (negative > positive || positive - negative > SIZE_MAX)
    return Result<std::size_t>(Status::failure(
        ErrorCode::InvalidArgument, "TensorSpan address precedes storage"));
  return Result<std::size_t>(static_cast<std::size_t>(positive - negative));
}

Status validate_tensor_geometry(const TensorDescriptor& descriptor,
                                const Region& region,
                                const StridedLayout& layout,
                                std::uint64_t bytes) {
  if (descriptor.shape.empty() || descriptor.shape.size() > 8 ||
      descriptor.shape.size() != layout.byte_strides.size() ||
      (!layout.origin.empty() &&
       layout.origin.size() != descriptor.shape.size()) ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](std::uint64_t extent) { return extent == 0; })) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "invalid tensor descriptor/layout");
  }
  auto status = region.validate(descriptor.shape);
  if (!status.ok())
    return status;
  std::size_t width = 0;
  try {
    width = TensorSpan::element_size(descriptor.element_type);
  } catch (const std::invalid_argument& error) {
    return Status::failure(ErrorCode::InvalidArgument, error.what());
  }
  if (!region.empty()) {
    std::array<std::uint64_t, 8> low{}, high{};
    for (std::size_t axis = 0; axis < region.rank(); ++axis) {
      auto first = region.dimensions()[axis].offset;
      auto last = first + region.dimensions()[axis].extent - 1;
      if (layout.byte_strides[axis] < 0)
        std::swap(first, last);
      low[axis] = first;
      high[axis] = last;
    }
    auto minimum = tensor_byte_address(layout, low.data(), region.rank());
    auto maximum = tensor_byte_address(layout, high.data(), region.rank());
    if (!minimum.ok())
      return minimum.status();
    if (!maximum.ok())
      return maximum.status();
    if (maximum.value() < minimum.value() || maximum.value() > bytes ||
        width > bytes - maximum.value()) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "tensor layout addresses outside storage");
    }
  } else if (layout.byte_offset > bytes) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "empty tensor offset exceeds storage");
  }
  return Status::success();
}
}  // namespace ps::data_internal
