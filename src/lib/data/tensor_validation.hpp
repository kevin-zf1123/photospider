#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "photospider/data/result.hpp"

namespace ps::input_internal {
/** @brief Exact 1..128-byte printable ASCII input name without spaces. */
bool valid_input_name(const std::string& name) noexcept;

/** @brief Checked dense metadata; never allocates a payload. */
struct DenseMetadata final {
  StridedLayout layout;
  std::uint64_t bytes = 0;
};

/** @brief Checks descriptor, signed canonical strides and host byte bounds. */
Result<DenseMetadata> dense_metadata(const TensorDescriptor& descriptor);

/** @brief Tests exact whole logical coverage without allocation. */
bool whole_region(const Region& region,
                  const std::vector<std::uint64_t>& shape) noexcept;
/** @brief Validate only scalar type, rank and nonzero dimensions. */
Status validate_value_structure(const TensorDescriptor& descriptor);
}  // namespace ps::input_internal
