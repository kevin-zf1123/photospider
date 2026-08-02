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
 * element semantics. The byte offset anchors logical coordinate zero within a
 * BufferHandle range. Producer builders accept positive strides and zero
 * offset only after proving that logical element byte ranges do not overlap;
 * immutable views may use checked positive, zero, or negative strides.
 *
 * @throws std::bad_alloc when copying the stride vector allocates and fails.
 * @note A StridedLayout is not independently valid. Value publication checks
 * rank, signed envelope arithmetic, writable non-overlap when applicable,
 * offset, and the complete retained range.
 */
struct StridedLayout {
  /** @brief Signed byte stride for each logical tensor axis. */
  std::vector<std::ptrdiff_t> byte_strides;

  /**
   * @brief Byte offset from BufferHandle range start to logical coordinate
   * zero.
   *
   * @note Producer builders require zero. Immutable sealed-buffer views may use
   * a checked nonzero offset, including the origin of a reverse layout.
   */
  std::size_t byte_offset = 0U;

  /**
   * @brief Compares every physical byte stride and the logical-origin offset.
   *
   * @param other Layout to compare.
   * @return True when both stride vectors and byte offsets are identical.
   * @throws Nothing under vector equality for ptrdiff_t elements.
   * @note Logical descriptor and storage ownership are intentionally excluded.
   */
  bool operator==(const StridedLayout& other) const noexcept {
    return byte_strides == other.byte_strides &&
           byte_offset == other.byte_offset;
  }
};

}  // namespace ps
