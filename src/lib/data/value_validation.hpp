#pragma once

#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "photospider/data/result.hpp"

namespace ps::input_internal {
/**
 * @brief Scoped binary32 nearest/gradual-underflow environment.
 * @note Floating state is thread-local. Save and restore the embedding's
 * rounding, denormal controls and exception flags even on callback failure.
 */
class Float32Environment final {
 public:
  Float32Environment() noexcept;
  ~Float32Environment() noexcept;
  Float32Environment(const Float32Environment&) = delete;
  Float32Environment& operator=(const Float32Environment&) = delete;
  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  std::fenv_t previous_{};
  bool saved_ = false;
  bool active_ = false;
};

/** @brief Exact 1..128-byte printable ASCII input name without spaces. */
bool valid_input_name(const std::string& name) noexcept;

/** @brief Checked dense metadata; never allocates a payload. */
struct DenseMetadata final {
  StridedLayout layout;
  std::uint64_t bytes = 0;
};

/** @brief Checks descriptor, signed canonical strides and host byte bounds. */
Result<DenseMetadata> dense_metadata(const ValueDescriptor& descriptor);

/** @brief Tests exact whole logical coverage without allocation. */
bool whole_region(const Region& region,
                  const std::vector<std::uint64_t>& shape) noexcept;
/** @brief Validate only scalar type, rank and nonzero dimensions. */
Status validate_value_structure(const ValueDescriptor& descriptor);
}  // namespace ps::input_internal
