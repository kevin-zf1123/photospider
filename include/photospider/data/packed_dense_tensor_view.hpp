#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "photospider/data/region.hpp"
#include "photospider/data/value.hpp"

/**
 * @file packed_dense_tensor_view.hpp
 * @brief Checked immutable access and exact slicing for V-13 packed tensors.
 */

namespace ps {

/**
 * @brief Retaining checked host view of one packed FP4 E2M1 DenseTensor.
 *
 * Construction retains the complete Value and a ReadLease. Element access
 * resolves the version-1 Blocked layout in bits, returns the stored nibble, and
 * applies the immutable row-major block scale only when dequantized access is
 * requested.
 *
 * @throws std::invalid_argument when construction receives an invalid,
 * non-Blocked, non-FP4, unquantized, or otherwise inconsistent Value.
 * @throws ReadyFenceAccessError when the retained Value is not Ready.
 * @throws BufferAccessError when the retained binding is not host-visible.
 * @note The view never returns a fake per-element byte pointer. Copy and
 * copy-like move operations retain the same immutable Value and allocation.
 */
class PackedDenseTensorView final {
 public:
  /**
   * @brief Retains one valid Ready host-visible V-13 packed Value.
   *
   * @param value Value copied or moved into the view.
   * @throws std::invalid_argument for invalid or unsupported packed facts.
   * @throws ReadyFenceAccessError when producer completion is not Ready.
   * @throws BufferAccessError when no direct host read is available.
   * @note Construction performs no implicit transfer, map, or conversion.
   */
  explicit PackedDenseTensorView(Value value);

  /** @brief Copies the retained immutable Value and ReadLease authority. */
  PackedDenseTensorView(const PackedDenseTensorView& other) noexcept = default;

  /**
   * @brief Copy-assigns another complete retaining view.
   * @param other View whose immutable state is shared.
   * @return This complete view.
   * @throws Nothing.
   */
  PackedDenseTensorView& operator=(
      const PackedDenseTensorView& other) noexcept = default;

  /**
   * @brief Move-constructs by sharing without invalidating the source.
   * @param other Complete source view.
   * @throws Nothing.
   */
  PackedDenseTensorView(PackedDenseTensorView&& other) noexcept
      : PackedDenseTensorView(other) {}

  /**
   * @brief Move-assigns by sharing without invalidating the source.
   * @param other Complete source view.
   * @return This complete view.
   * @throws Nothing.
   */
  PackedDenseTensorView& operator=(PackedDenseTensorView&& other) noexcept {
    return *this = other;
  }

  /**
   * @brief Returns the retained immutable Value.
   * @return Borrowed Value handle.
   * @throws Nothing.
   */
  const Value& value() const noexcept;

  /**
   * @brief Returns the retained logical descriptor.
   * @return Borrowed validated FP4 descriptor and quantization schema.
   * @throws Nothing after successful construction.
   */
  const DenseTensorDescriptor& descriptor() const noexcept;

  /**
   * @brief Returns the retained physical layout.
   * @return Borrowed validated version-1 Blocked layout.
   * @throws Nothing after successful construction.
   */
  const BlockedLayout& layout() const noexcept;

  /**
   * @brief Returns one exact four-bit E2M1 storage code.
   *
   * @param coordinates One coordinate for every logical tensor axis.
   * @return Unsigned value in the inclusive range zero through fifteen.
   * @throws std::invalid_argument when coordinate rank differs from tensor
   *         rank.
   * @throws std::out_of_range when any coordinate exceeds its logical extent.
   * @throws std::overflow_error if checked coordinate arithmetic overflows.
   * @note Successful Value validation proves the selected nibble is inside the
   *       retained ReadLease range.
   */
  std::uint8_t encoded_element(
      const std::vector<std::size_t>& coordinates) const;

  /**
   * @brief Decodes and scale-dequantizes one FP4 E2M1 element.
   *
   * @param coordinates One coordinate for every logical tensor axis.
   * @return Signed E2M1 value multiplied by its logical block's scale.
   * @throws std::invalid_argument, std::out_of_range, or std::overflow_error
   *         from checked coordinate and block lookup.
   * @note The result is an explicit read calculation; it does not publish a
   *       converted Value or new revision.
   */
  float dequantized_element(const std::vector<std::size_t>& coordinates) const;

 private:
  /** @brief Complete retained Value establishing metadata lifetime. */
  Value value_;

  /** @brief Retaining host read lease establishing byte-pointer lifetime. */
  ReadLease read_lease_;
};

/**
 * @brief Copies one exact quantization-block-aligned TensorSlice while packed.
 *
 * @param source Ready host-visible V-13 FP4 Blocked Value.
 * @param slice Matching-domain, full-rank, nonempty, block-aligned selection.
 * @return Fresh Ready CPU Value with sliced shape/scales, a canonical
 *         contiguous Blocked layout, preserved bit order and bit offset, and
 *         directly copied four-bit codes.
 * @throws std::invalid_argument for invalid source facts, wrong Region domain
 *         or rank, empty/reversed/out-of-bounds intervals, or endpoints that
 *         cut a quantization block.
 * @throws ReadyFenceAccessError or BufferAccessError when source bytes are not
 *         directly readable.
 * @throws std::overflow_error when block, bit, byte, or coordinate arithmetic
 *         cannot be represented.
 * @throws std::bad_alloc when scale, layout, coordinate, or output storage
 *         allocation fails.
 * @note The operation never dequantizes, requantizes, widens, or adapts through
 *       ImageBuffer. Output allocation and Value revision are always fresh.
 */
Value copy_packed_dense_tensor_slice(const Value& source,
                                     const TensorSlice& slice);

}  // namespace ps
