#pragma once

#include <cstdint>

namespace ps::execution_internal {
// Logical publication cardinality can exceed uint64 even when a zero-stride
// view has bounded physical backing. Keep that loss of precision explicit.
inline void add_computed_elements(std::uint64_t* total, bool* saturated,
                                  std::uint64_t count,
                                  bool count_saturated = false) noexcept {
  if (*saturated || count_saturated || count > UINT64_MAX - *total) {
    *total = UINT64_MAX;
    *saturated = true;
  } else {
    *total += count;
  }
}
}  // namespace ps::execution_internal
