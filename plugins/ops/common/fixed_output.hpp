#pragma once

#include <cstdint>

#include "photospider/plugin/fixed_invocation_api.h"

namespace ps::plugin_internal::fixed_tensor {
// Offsets relative to a mutable view's byte_offset. Its validated strides
// describe the physical tile even when logical HWC channels occupy planes.
inline std::uint64_t coordinate_offset(const ps_kernel_layout_v8& layout,
                                       const std::uint64_t* at) noexcept {
  std::uint64_t offset = 0;
  for (unsigned a = 0; a < layout.rank; ++a)
    offset += (at[a] - layout.origin[a]) * layout.strides[a];
  return offset;
}
inline std::uint64_t sample_offset(const ps_kernel_layout_v8& layout,
                                   std::uint64_t index) noexcept {
  std::uint64_t offset = 0;
  for (unsigned a = layout.rank; a-- > 0;) {
    offset += (index % layout.shape[a]) * layout.strides[a];
    index /= layout.shape[a];
  }
  return offset;
}
inline bool packed_output(const ps_kernel_layout_v8& layout,
                          std::uint64_t width) noexcept {
  for (unsigned a = layout.rank; a-- > 0;) {
    if (layout.shape[a] > 1 &&
        layout.strides[a] != static_cast<std::int64_t>(width))
      return false;
    width *= layout.shape[a];
  }
  return true;
}
}  // namespace ps::plugin_internal::fixed_tensor
