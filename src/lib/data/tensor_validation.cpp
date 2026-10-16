#include "data/tensor_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dense_layout_validation.hpp"
#include "photospider/core/checked_math.hpp"

namespace ps::input_internal {
namespace {
Status failure(ErrorCode code, const char* message) {
  return Status::failure(code, message);
}
}  // namespace
bool valid_input_name(const std::string& name) noexcept {
  return !name.empty() && name.size() <= 128 &&
         std::none_of(name.begin(), name.end(), [](unsigned char byte) {
           return byte < 0x21 || byte > 0x7e;
         });
}

Result<DenseMetadata> dense_metadata(const TensorDescriptor& descriptor) {
  if (descriptor.shape.empty() || descriptor.shape.size() > 8 ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](std::uint64_t extent) { return extent == 0; })) {
    return Result<DenseMetadata>(
        failure(ErrorCode::InvalidArgument, "invalid dense descriptor shape"));
  }
  std::uint64_t stride = 0;
  try {
    stride = TensorSpan::element_size(descriptor.element_type);
  } catch (const std::invalid_argument&) {
    return Result<DenseMetadata>(
        failure(ErrorCode::InvalidArgument, "unknown dense element type"));
  }
  DenseMetadata result;
  result.layout.byte_strides.resize(descriptor.shape.size());
  for (std::size_t reverse = descriptor.shape.size(); reverse > 0; --reverse) {
    const auto axis = reverse - 1;
    if (stride > INT64_MAX ||
        !core_internal::can_multiply(stride, descriptor.shape[axis])) {
      return Result<DenseMetadata>(failure(ErrorCode::ResourceExhausted,
                                           "dense stride or product overflow"));
    }
    result.layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
    stride *= descriptor.shape[axis];
  }
  if (!core_internal::dense_byte_size_representable<std::size_t>(stride)) {
    return Result<DenseMetadata>(failure(
        ErrorCode::ResourceExhausted, "dense bytes are not host addressable"));
  }
  result.bytes = stride;
  return Result<DenseMetadata>(std::move(result));
}

bool whole_region(const Region& region,
                  const std::vector<std::uint64_t>& shape) noexcept {
  if (region.rank() != shape.size())
    return false;
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (region.dimensions()[i].offset != 0 ||
        region.dimensions()[i].extent != shape[i])
      return false;
  }
  return true;
}

Status validate_value_structure(const TensorDescriptor& descriptor) {
  const auto element = static_cast<std::uint32_t>(descriptor.element_type);
  if (element < 1 || element > 7 || descriptor.shape.empty() ||
      descriptor.shape.size() > 8 ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](auto n) { return n == 0; }))
    return failure(ErrorCode::TypeMismatch, "invalid port descriptor");
  return Status::success();
}
}  // namespace ps::input_internal
