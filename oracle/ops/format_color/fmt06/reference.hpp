#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Independent FMT-06 references extracted from test_numeric_conversion.cpp.
// Float input is binary32 in [0,1]; execute with FE_TONEAREST. No production
// conversion or operation execution code is used.
namespace conversion_oracle {
inline std::uint8_t float32_to_uint8(float sample) {
  const double exact = static_cast<double>(sample) * 255;
  const auto floor = static_cast<unsigned>(exact);
  const auto fraction = exact - floor;
  return static_cast<std::uint8_t>(
      floor + (fraction > 0.5 || (fraction == 0.5 && (floor & 1))));
}
inline std::uint8_t int64_to_uint8(std::int64_t sample) {
  const auto offset = static_cast<std::uint64_t>(sample) - (UINT64_C(1) << 63);
  const auto numerator = static_cast<unsigned __int128>(offset) * 255;
  const auto quotient = numerator / UINT64_MAX;
  const auto remainder = numerator % UINT64_MAX;
  const auto expected =
      quotient + (remainder * 2 > UINT64_MAX ||
                  (remainder * 2 == UINT64_MAX && (quotient & 1)));
  return static_cast<std::uint8_t>(expected);
}
// count >= 765 retains every threshold and its adjacent binary32 values.
inline std::vector<float> float32_inputs(std::uint64_t count = 1024) {
  std::vector<float> samples(count);
  std::uint32_t random = UINT32_C(0x243f6a88);
  for (std::uint64_t i = 0; i < count; ++i) {
    random = random * UINT32_C(1664525) + UINT32_C(1013904223);
    const auto bits = random % UINT32_C(0x3f800001);
    std::memcpy(&samples[i], &bits, 4);
  }
  for (unsigned i = 0; i < 255; ++i) {
    const auto value = static_cast<float>((i + 0.5) / 255.0);
    const float values[] = {std::nextafter(value, 0.0f), value,
                            std::nextafter(value, 1.0f)};
    for (unsigned j = 0; j < 3; ++j)
      samples.at(i * 3 + j) = values[j];
  }
  return samples;
}
inline std::vector<std::int64_t> int64_inputs(std::uint64_t count = 1024) {
  std::vector<std::int64_t> samples(count);
  std::uint64_t state = UINT64_C(0x243f6a8885a308d3);
  for (auto& sample : samples) {
    state = state * UINT64_C(6364136223846793005) + 1;
    std::memcpy(&sample, &state, 8);
  }
  return samples;
}
}  // namespace conversion_oracle
