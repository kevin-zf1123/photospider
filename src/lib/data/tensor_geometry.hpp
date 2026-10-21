#pragma once

#include "photospider/data/tensor_span.hpp"

namespace ps::data_internal {
/** @brief Checked affine geometry over an opaque byte extent; no CPU access. */
Status validate_tensor_geometry(const TensorDescriptor& descriptor,
                                const Region& region,
                                const StridedLayout& layout,
                                std::uint64_t bytes);
/** @brief Checked origin-relative byte index; caller provides validated ranks.
 */
Result<std::size_t> tensor_byte_address(const StridedLayout& layout,
                                        const std::uint64_t* coordinate,
                                        std::size_t rank);
}  // namespace ps::data_internal
