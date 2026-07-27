#pragma once

#include <cstddef>
#include <vector>

/**
 * @file strided_layout.hpp
 * @brief Dependency-neutral physical byte-stride contract for tensor data.
 */

namespace ps {

/**
 * @brief Describes the physical byte distance between logical tensor axes.
 *
 * The stride vector is deliberately separate from logical tensor shape and
 * element semantics. V-2 accepts only positive strides, while the signed
 * vocabulary leaves later read-only offset and reverse-layout work source
 * compatible.
 *
 * @throws std::bad_alloc when copying the stride vector allocates and fails.
 * @note A StridedLayout is not independently valid. Value construction checks
 *       rank, positivity, arithmetic, and the complete owned storage envelope.
 */
struct StridedLayout {
  /** @brief Signed byte stride for each logical tensor axis. */
  std::vector<std::ptrdiff_t> byte_strides;

  /**
   * @brief Compares every physical byte stride.
   *
   * @param other Layout to compare.
   * @return True when both stride vectors are identical.
   * @throws Nothing under vector equality for ptrdiff_t elements.
   * @note Logical descriptor and storage ownership are intentionally excluded.
   */
  bool operator==(const StridedLayout& other) const noexcept {
    return byte_strides == other.byte_strides;
  }
};

}  // namespace ps
