#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Independent straight-alpha bilinear reference. Use FE_TONEAREST, gradual
// underflow and -fno-fast-math -ffp-contract=off. Coordinates must fit int64_t
// after floor; source is nonempty packed RGBA with height * width pixels.
namespace stmap_oracle {
inline std::int64_t stmap_reference_index(std::int64_t i, std::int64_t n,
                                          const std::string& mode) {
  if (i >= 0 && i < n)
    return i;
  if (mode == "constant")
    return -1;
  if (mode == "clamp")
    return std::max<std::int64_t>(0, std::min(n - 1, i));
  if (n == 1)
    return 0;
  const auto period = mode == "wrap"      ? n
                      : mode == "reflect" ? 2 * n
                                          : 2 * (n - 1);
  const auto r = (i % period + period) % period;
  return mode == "wrap" ? r
                        : std::min(r, period - (mode == "reflect" ? 1 : 0) - r);
}
inline std::array<float, 4> stmap_reference(const std::vector<float>& source,
                                            std::uint64_t height,
                                            std::uint64_t width, double u,
                                            double v, const std::string& mode) {
  // An independent scalar oracle with explicit rounding after every operation;
  // it cannot inherit the production loop's vectorization or FMA decisions.
  volatile double sx = u - .5, sy = v - .5;
  const auto left = static_cast<std::int64_t>(std::floor(sx));
  const auto top = static_cast<std::int64_t>(std::floor(sy));
  volatile double fx = sx - static_cast<double>(left),
                  fy = sy - static_cast<double>(top);
  std::array<double, 4> weights{};
  std::array<std::array<float, 4>, 4> values{};
  for (unsigned dy = 0; dy < 2; ++dy)
    for (unsigned dx = 0; dx < 2; ++dx) {
      const auto k = dy * 2 + dx;
      volatile double weight = (dy ? fy : 1 - fy) * (dx ? fx : 1 - fx);
      weights[k] = weight;
      const auto y = stmap_reference_index(top + dy, height, mode);
      const auto x = stmap_reference_index(left + dx, width, mode);
      if (x >= 0 && y >= 0) {
        for (std::size_t c = 0; c < 4; ++c)
          values[k][c] = source[(y * width + x) * 4 + c];
      }
    }
  std::array<double, 4> sums{};
  for (std::size_t k = 0; k < 4; ++k) {
    volatile double alpha_weight =
        static_cast<double>(values[k][3]) * weights[k];
    volatile double alpha = sums[3] + alpha_weight;
    sums[3] = alpha;
    if (alpha_weight == 0)
      continue;
    for (std::size_t c = 0; c < 3; ++c) {
      volatile double product =
          static_cast<double>(values[k][c]) * alpha_weight;
      volatile double next = sums[c] + product;
      sums[c] = next;
    }
  }
  std::array<float, 4> result{};
  volatile float alpha = static_cast<float>(sums[3]);
  result[3] = alpha;
  for (std::size_t c = 0; c < 3; ++c) {
    if (sums[3] == 0) {
      result[c] = 0.0F;
    } else {
      volatile double straight = sums[c] / sums[3];
      volatile float narrowed = static_cast<float>(straight);
      result[c] = narrowed;
    }
  }
  return result;
}
// Boundary, subnormal and large-exponent inputs from the original oracle.
// Evaluate the first h*w pixels for (h,w) in {(2,3), (1,1), (1,2)} and all five
// modes: constant, clamp, wrap, reflect, mirror.
inline std::pair<std::vector<float>, std::vector<double>> sample_inputs() {
  const auto tiny = std::numeric_limits<float>::denorm_min();
  const auto normal = std::numeric_limits<float>::min();
  const std::vector<float> original{
      -1,     -0.F,  tiny,      1,        std::nextafter(1.F, 2.F),
      normal, -tiny, 1,         0x1p80F,  -0x1p80F,
      .25F,   1,     -0x1p80F,  0x1p80F,  -.25F,
      1,      1,     -1,        3 * tiny, 1,
      -1,     1,     -3 * tiny, 1};
  const std::vector<double> uv{.5,   .5,      1,      .5,  1.75,    1.25, -3.25,
                               4.75, 0x1p-56, .5,     3.5, 2.5,     -.5,  -.5,
                               1.5,  1.5,     0x1p40, .5,  -0x1p40, .5};
  return {original, uv};
}
}  // namespace stmap_oracle
