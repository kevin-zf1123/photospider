#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace ps::plugin_internal {

/**
 * @brief Largest signed last-byte offset accepted by a dense Value layout.
 *
 * @note The unsigned literal avoids overflowing signed arithmetic when a legal
 * dense byte count is exactly one greater than `INT64_MAX`.
 */
constexpr std::uint64_t kMaxDenseOffset = UINT64_C(9223372036854775807);

/**
 * @brief Converts one unsigned host-size type maximum without truncation.
 * @tparam SizeType Unsigned integral allocation-size type.
 * @return `SizeType` maximum capped at `UINT64_MAX`.
 * @throws Nothing.
 * @note The compile-time width branch keeps 32-bit and wider-than-64-bit hosts
 * well-formed without narrowing a discarded maximum.
 */
template <typename SizeType>
constexpr std::uint64_t size_type_maximum_as_uint64() noexcept {
  static_assert(std::is_integral<SizeType>::value,
                "dense size type must be integral");
  static_assert(std::is_unsigned<SizeType>::value,
                "dense size type must be unsigned");
  if constexpr (sizeof(SizeType) >= sizeof(std::uint64_t)) {
    return std::numeric_limits<std::uint64_t>::max();
  } else {
    return static_cast<std::uint64_t>(std::numeric_limits<SizeType>::max());
  }
}

/**
 * @brief Validates a complete dense byte range for one host-size type.
 * @tparam SizeType Unsigned integral allocation-size type, normally `size_t`.
 * @param byte_size Complete dense byte count after checked stride products.
 * @return True only when the byte count is positive, its last byte offset fits
 * signed `int64_t`, and the complete allocation count fits `SizeType`.
 * @throws Nothing.
 * @note The zero check precedes `byte_size - 1`; no signed addition or
 * multiplication is used. Tests instantiate `uint32_t` to cover the 32-bit
 * host-size path on a 64-bit builder.
 */
template <typename SizeType>
constexpr bool dense_byte_size_representable(std::uint64_t byte_size) noexcept {
  return byte_size != 0U && byte_size - UINT64_C(1) <= kMaxDenseOffset &&
         byte_size <= size_type_maximum_as_uint64<SizeType>();
}

}  // namespace ps::plugin_internal
