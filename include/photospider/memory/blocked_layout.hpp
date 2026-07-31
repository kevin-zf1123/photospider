#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file blocked_layout.hpp
 * @brief Dependency-neutral bit-addressed blocked tensor layout contracts.
 */

namespace ps {

/**
 * @brief Selects how a packed element's low-order value bits occupy one byte.
 *
 * @throws Nothing for ordinary value operations.
 * @note V-13 validates nibble-aligned FP4 positions, so an element never
 *       crosses a byte boundary. This enum describes packing only; numeric
 *       element encoding remains a separate descriptor fact.
 */
enum class PackedBitOrder : std::uint32_t {
  /** @brief Bit offset zero selects the least-significant nibble. */
  LeastSignificantFirst = 0U,
  /** @brief Bit offset zero selects the most-significant nibble. */
  MostSignificantFirst = 1U,
};

/**
 * @brief Describes a versioned bit-addressed row-major-in-block layout.
 *
 * Each logical coordinate is split by `block_shape` into a block coordinate
 * and a coordinate inside that block. Block coordinates use
 * `block_bit_strides`; elements inside each block use row-major order and the
 * descriptor's physical bit width. `bit_offset` anchors logical coordinate
 * zero within the retained BufferHandle range.
 *
 * @throws std::bad_alloc when copying either vector allocates and fails.
 * @note A BlockedLayout is not independently valid. V-13 Value publication
 *       accepts version 1 FP4 layouts only after proving matching ranks and
 *       quantization blocks, nibble alignment, exact byte bounds, and
 *       non-overlapping complete block ranges.
 */
struct BlockedLayout {
  /** @brief Layout contract version; V-13 supports exactly version 1. */
  std::uint32_t version = 1U;

  /** @brief Positive logical element extents inside one physical block. */
  std::vector<std::size_t> block_shape;

  /** @brief Positive bit stride for each logical block-grid axis. */
  std::vector<std::size_t> block_bit_strides;

  /** @brief Absolute bit offset of logical coordinate zero in the buffer. */
  std::size_t bit_offset = 0U;

  /** @brief Explicit order of packed value bits within each storage byte. */
  PackedBitOrder bit_order = PackedBitOrder::LeastSignificantFirst;

  /**
   * @brief Compares the complete physical blocked layout.
   *
   * @param other Layout to compare.
   * @return True when version, shape, strides, offset, and order all match.
   * @throws Nothing under vector equality for size_t elements.
   * @note Logical descriptor, quantization values, and storage ownership are
   *       intentionally excluded.
   */
  bool operator==(const BlockedLayout& other) const noexcept {
    return version == other.version && block_shape == other.block_shape &&
           block_bit_strides == other.block_bit_strides &&
           bit_offset == other.bit_offset && bit_order == other.bit_order;
  }
};

}  // namespace ps
