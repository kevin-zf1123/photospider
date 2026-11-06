#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "photospider/core/checked_math.hpp"
#include "photospider/data/tensor_span.hpp"

namespace ps::data_internal {
// The sorted stride magnitudes must cover exactly the authorized bytes. A
// min/max address check alone would accept overlapping samples with a hole.
inline Result<ByteView> physical_piece(const TensorSpan& span) {
  const auto& dims = span.region().dimensions();
  const auto& strides = span.layout().byte_strides;
  std::array<std::uint64_t, 8> first{};
  std::array<std::pair<std::uint64_t, std::uint64_t>, 8> axes{};
  std::size_t used = 0;
  for (std::size_t axis = 0; axis < dims.size(); ++axis) {
    const auto d = dims[axis];
    first[axis] = d.offset;
    if (d.extent == 1)
      continue;
    const auto stride = strides[axis];
    if (stride < 0)
      first[axis] += d.extent - 1;
    const auto magnitude =
        stride < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(stride)
                   : static_cast<std::uint64_t>(stride);
    axes[used++] = {magnitude, d.extent};
  }
  std::sort(axes.begin(), axes.begin() + used);
  std::uint64_t length =
      TensorSpan::element_size(span.descriptor().element_type);
  for (std::size_t i = 0; i < used; ++i) {
    if (axes[i].first != length)
      return Result<ByteView>({ErrorCode::InvalidArgument,
                               "tensor piece requires layout conversion",
                               FailureReason::ViewUnavailable});
    if (!core_internal::checked_multiply(length, axes[i].second, &length))
      return Result<ByteView>(
          {ErrorCode::ResourceExhausted, "physical tile size overflow"});
  }
  auto base = span.byte_address(first.data(), dims.size());
  if (!base.ok())
    return Result<ByteView>(base.status());
  return Result<ByteView>(ByteView(span.bytes().data() + base.value(), length));
}
}  // namespace ps::data_internal
