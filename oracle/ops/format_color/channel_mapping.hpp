#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// Independent packed-byte mapping extracted from channel_result_fixture.hpp.
// Coordinates, axes, mapping and raw storage must satisfy their declared
// shapes. Descriptor shapes remain separate from sample shapes so scalar [1]
// handling retains the original rule even when batch axes contribute to sample
// shape.
namespace channel_oracle {
inline std::vector<std::uint8_t> expected_bytes(
    const std::vector<std::vector<std::uint64_t>>& coordinates,
    std::uint32_t output_axis,
    const std::vector<std::optional<std::uint32_t>>& source_axes,
    const std::vector<std::pair<unsigned, unsigned>>& mapping,
    const std::vector<std::vector<std::uint64_t>>& descriptor_shapes,
    const std::vector<std::vector<std::uint64_t>>& sample_shapes,
    const std::vector<std::vector<std::uint8_t>>& raw, std::uint64_t width) {
  std::vector<std::uint8_t> expected;
  for (const auto& coordinate : coordinates) {
    const auto selected = mapping.at(coordinate[output_axis]);
    auto at = coordinate;
    at.erase(at.begin() + output_axis);
    if (source_axes[selected.first])
      at.insert(at.begin() + *source_axes[selected.first], selected.second);
    else if (descriptor_shapes[selected.first] == std::vector<std::uint64_t>{1})
      at = {0};
    std::uint64_t source = 0;
    for (std::size_t i = 0; i < at.size(); ++i)
      source = source * sample_shapes[selected.first][i] + at[i];
    const auto begin = raw[selected.first].begin() + source * width;
    expected.insert(expected.end(), begin, begin + width);
  }
  return expected;
}
}  // namespace channel_oracle
