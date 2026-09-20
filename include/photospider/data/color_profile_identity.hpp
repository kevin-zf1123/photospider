#pragma once

#include <array>
#include <cstdint>

#include "photospider/core/export.hpp"

namespace ps {
/** @brief SHA-256 of immutable ICC bytes and their exact length, not an owner.
 */
struct PHOTOSPIDER_API ColorProfileIdentity final {
  std::uint64_t byte_length = 0;
  std::array<std::uint8_t, 32> sha256{};
  bool operator==(const ColorProfileIdentity& other) const noexcept {
    return byte_length == other.byte_length && sha256 == other.sha256;
  }
  /** @brief Deterministic metadata ordering, independent of allocation
   * identity. */
  bool operator<(const ColorProfileIdentity& other) const noexcept {
    return byte_length != other.byte_length ? byte_length < other.byte_length
                                            : sha256 < other.sha256;
  }
};
}  // namespace ps
