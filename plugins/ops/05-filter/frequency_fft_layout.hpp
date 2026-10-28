#pragma once

#include <algorithm>
#include <cstdint>

namespace ps::plugin_internal::frequency {
// Factor only dimensions. A large residual is convolved at the leaf, rather
// than padding the complete composite axis to the next power of two.
inline std::uint64_t transform_factor(std::uint64_t n) {
  for (auto r : {4U, 2U, 3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U})
    if (n % r == 0)
      return r;
  // Bound static factor discovery independently of caller work limits.
  for (auto r :
       {37U,  41U,  43U,  47U,  53U,  59U,  61U,  67U,  71U,  73U,  79U,
        83U,  89U,  97U,  101U, 103U, 107U, 109U, 113U, 127U, 131U, 137U,
        139U, 149U, 151U, 157U, 163U, 167U, 173U, 179U, 181U, 191U, 193U,
        197U, 199U, 211U, 223U, 227U, 229U, 233U, 239U, 241U, 251U, 257U})
    if (n % r == 0)
      return r;
  return n;
}
inline bool transform_smooth(std::uint64_t n) {
  while (n > 1) {
    const auto r = transform_factor(n);
    if (r > 31)
      return false;
    n /= r;
  }
  return true;
}
inline std::uint64_t transform_leaf_capacity(std::uint64_t n) {
  std::uint64_t size = 1;
  while (size < 2 * n - 1)
    size *= 2;
  return size;
}
// Bound both a leaf convolution and a proper large-radix stage's gathered
// values plus its convolution. Hardware and cache state do not affect this.
inline std::uint64_t transform_convolution_capacity(std::uint64_t n) {
  std::uint64_t extra = 0;
  while (n > 1) {
    const auto r = transform_factor(n);
    if (r > 31) {
      if (r == n)
        return std::max(extra, transform_leaf_capacity(n));
      extra = std::max(extra, r + transform_leaf_capacity(r));
    }
    n /= r;
  }
  return extra;
}

inline constexpr unsigned kTransformWorkers = 32;
inline std::uint64_t transform_worker_capacity(std::uint64_t h,
                                               std::uint64_t pitch,
                                               std::uint64_t row) {
  if (h * pitch < 4096)
    return 1;
  const auto lines = std::max(row > 1 ? h : 1, h > 1 ? pitch : 1);
  // Small prime axes can amortize parallel work before a 128-square plane.
  const auto limit = h * pitch < 16384 ? 8U : kTransformWorkers;
  return std::min<std::uint64_t>(limit, lines);
}
inline std::uint64_t transform_line_workspace(std::uint64_t h,
                                              std::uint64_t row) {
  return 8 * (std::max(h, row) + std::max(transform_convolution_capacity(h),
                                          transform_convolution_capacity(row)));
}
inline bool transform_double_columns(std::uint64_t n) {
  if (n < 8 || (n & (n - 1)))
    return false;
  bool selected =
      n <= 128 || n == 1024 || n == 2048 || (n >= 4096 && n <= 32768);
#if defined(__x86_64__)
  selected |= n == 256 || n == 512;
#endif
  return selected;
}
inline std::uint64_t transform_column_workspace(std::uint64_t h,
                                                std::uint64_t pitch,
                                                bool double_precision) {
  if (h < 256 || pitch <= 1 ||
      (double_precision && transform_double_columns(h)))
    return 0;
  const bool power = !(h & (h - 1));
  if (!double_precision && power && h <= 65536 && pitch % 4 == 0) {
#if defined(__APPLE__) && defined(__aarch64__)
    // Sixteen native float columns need twice the eight-line base workspace.
    return 8 * h;
#else
    return 0;
#endif
  }
  // Strided mixed transforms below this boundary use the line workspace.
  if (h < 8192 && !power)
    return 0;
#if defined(__x86_64__)
  constexpr std::uint64_t power_limit = 4096;
  const std::uint64_t maximum_band = h >= 4096 ? 8 : 64;
#else
  constexpr std::uint64_t power_limit = 8192;
  constexpr std::uint64_t maximum_band = 64;
#endif
  // Both packet widths can transform these bands in the line workspace.
  auto extra = power && pitch % 4 == 0 && h < power_limit
                   ? 0
                   : std::min(pitch, maximum_band) * (h + 1);
  return extra;
}
inline std::uint64_t transform_worker_stride(std::uint64_t h,
                                             std::uint64_t pitch,
                                             std::uint64_t row,
                                             bool double_precision) {
  const auto n = std::max(h, row);
  // One contiguous power-of-two axis has one scratch slice. The two-factor
  // path uses N shared values and at most N values across worker bands.
  if ((h == 1 || (pitch == 1 && row == 1)) && n >= 16384 && n <= 65536 &&
      !(n & (n - 1)))
    return 2 * n;
  return (transform_line_workspace(h, row) +
          transform_column_workspace(h, pitch, double_precision) + 7) /
         8 * 8;
}
}  // namespace ps::plugin_internal::frequency
