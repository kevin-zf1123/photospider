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
/** @brief Fixed logical validation domain; no unbounded coordinate allocation.
 * All extents are positive and first+extent must be representable. The output
 * and rank use the same observation coordinate space as AtomKey. This range
 * names semantic scope, not an expanded physical source read.
 */
struct AtomDomain final {
  AtomKey first = {};
  std::array<std::uint64_t, 8> extent{};
  bool canonical() const noexcept {
    if (!first.canonical())
      return false;
    for (std::uint32_t i = 0; i < 8; ++i) {
      if (i < first.rank) {
        if (!extent[i] || extent[i] > UINT64_MAX - first.coordinate[i])
          return false;
      } else if (extent[i]) {
        return false;
      }
    }
    return true;
  }
  bool contains(const AtomKey& key) const noexcept {
    if (!canonical() || !key.canonical() ||
        key.output_index != first.output_index || key.rank != first.rank)
      return false;
    for (std::uint32_t i = 0; i < first.rank; ++i)
      if (key.coordinate[i] < first.coordinate[i] ||
          key.coordinate[i] - first.coordinate[i] >= extent[i])
        return false;
    return true;
  }
};
}  // namespace ps
