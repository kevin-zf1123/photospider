#pragma once

#include <array>
#include <cstdint>

namespace ps::plugin_internal::mask {
// Small invocation-local memo for pure scalar strict calculations. Every key
// stores complete input bit patterns; a hash collision never substitutes a
// result. The owning operator includes this fixed storage in admitted scratch.
template <unsigned Words>
struct ScalarMemo final {
  struct Entry final {
    std::array<std::uint64_t, Words> key{};
    std::uint64_t value = 0;
    bool valid = false;
  };
  std::array<std::array<Entry, 4>, 64> sets{};
  std::array<unsigned, 64> next{};
  template <class Function>
  std::uint64_t evaluate(const std::array<std::uint64_t, Words>& key,
                         Function calculate) {
    std::uint64_t hash = UINT64_C(0x9e3779b97f4a7c15);
    for (auto word : key) {
      word ^= word >> 30;
      word *= UINT64_C(0xbf58476d1ce4e5b9);
      word ^= word >> 27;
      word *= UINT64_C(0x94d049bb133111eb);
      hash ^= word ^ (word >> 31);
      hash = (hash << 17) | (hash >> 47);
    }
    const auto index = hash & 63;
    auto& set = sets[index];
    for (const auto& entry : set)
      if (entry.valid && entry.key == key)
        return entry.value;
    const auto value = calculate();
    auto& entry = set[next[index]];
    next[index] = (next[index] + 1) & 3;
    entry = {key, value, true};
    return value;
  }
};
}  // namespace ps::plugin_internal::mask
