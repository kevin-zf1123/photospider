#pragma once

#include <array>
#include <cstdint>

namespace ps {
/** @brief One output observation, independent of request partition and storage.
 * Coordinates use the operation's observation domain (HW for image pixels).
 * rank is 1..8 and unused coordinates must be zero. output_index is the
 * declaration-order output of the producing operation, never a batch slot.
 */
struct AtomKey final {
  std::uint32_t output_index = 0, rank = 0;
  std::array<std::uint64_t, 8> coordinate{};
  bool canonical() const noexcept {
    if (output_index >= 64 || rank == 0 || rank > 8)
      return false;
    for (std::uint32_t i = rank; i < 8; ++i)
      if (coordinate[i])
        return false;
    return true;
  }
  bool operator==(const AtomKey& other) const noexcept {
    return output_index == other.output_index && rank == other.rank &&
           coordinate == other.coordinate;
  }
  bool operator!=(const AtomKey& other) const noexcept {
    return !(*this == other);
  }
  bool operator<(const AtomKey& other) const noexcept {
    if (output_index != other.output_index)
      return output_index < other.output_index;
    if (rank != other.rank)
      return rank < other.rank;
    return coordinate < other.coordinate;
  }
};
}  // namespace ps
