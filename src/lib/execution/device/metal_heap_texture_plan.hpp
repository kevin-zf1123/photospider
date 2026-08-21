#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

/**
 * @file metal_heap_texture_plan.hpp
 * @brief Checked dependency-neutral planning for one Metal heap texture.
 */

namespace ps::execution {

/**
 * @brief Validated native requirements for one dedicated heap texture.
 *
 * @throws Nothing for aggregate construction, copying, and destruction.
 * @note `heap_size` is the admitted size passed to `MTLHeapDescriptor`; it is
 * never an estimate for a direct `MTLDevice::newTextureWithDescriptor`
 * allocation.
 */
struct MetalHeapTexturePlan final {
  /** @brief Native heap-suballocation byte requirement. */
  std::uint64_t required_size = 0U;

  /** @brief Positive power-of-two native placement alignment. */
  std::uint64_t required_alignment = 0U;

  /** @brief Required size conservatively rounded to native alignment. */
  std::uint64_t heap_size = 0U;
};

/**
 * @brief Validates and rounds one native heap texture size/alignment query.
 * @param required_size Positive heap-suballocation byte requirement.
 * @param required_alignment Positive power-of-two placement alignment.
 * @return Complete plan whose heap size covers the queried requirement.
 * @throws std::invalid_argument for zero size or a zero/non-power-of-two
 * alignment.
 * @throws std::overflow_error when alignment rounding is unrepresentable.
 * @note The helper allocates nothing and performs no native call. Callers must
 * use the resulting plan only with a heap-backed texture whose descriptor is
 * identical to the queried descriptor.
 */
inline MetalHeapTexturePlan checked_metal_heap_texture_plan(
    std::uint64_t required_size, std::uint64_t required_alignment) {
  if (required_size == 0U) {
    throw std::invalid_argument(
        "Metal heap texture plan requires a positive size.");
  }
  if (required_alignment == 0U ||
      (required_alignment & (required_alignment - 1U)) != 0U) {
    throw std::invalid_argument(
        "Metal heap texture plan requires power-of-two alignment.");
  }
  const std::uint64_t remainder = required_size % required_alignment;
  if (remainder == 0U) {
    return MetalHeapTexturePlan{required_size, required_alignment,
                                required_size};
  }
  const std::uint64_t padding = required_alignment - remainder;
  if (required_size > std::numeric_limits<std::uint64_t>::max() - padding) {
    throw std::overflow_error("Metal heap texture plan size overflow.");
  }
  return MetalHeapTexturePlan{required_size, required_alignment,
                              required_size + padding};
}

}  // namespace ps::execution
