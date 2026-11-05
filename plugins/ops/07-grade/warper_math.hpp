// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::grade::warper {
// Keep the overflow-safe alternative out of the ordinary interpolation path.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline double scaled_fraction(double x, double a, double b) {
  return (x * .5 - a * .5) / (b * .5 - a * .5);
}
inline double fraction(double x, double a, double b) {
  if (x <= a)
    return 0;
  if (x >= b)
    return 1;
  const double d = b - a;
  if (std::isfinite(d))
    return (x - a) / d;
  return scaled_fraction(x, a, b);
}
inline double lerp(double a, double b, double t) {
  if (t == 0 || a == b)
    return a;
  if (t == 1.0)
    return b;
  return (1.0 - t) * a + t * b;
}
struct Cell {
  unsigned first, second;
  double fraction, coordinate;
};
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline unsigned interior_cell(double x, const double* axis, unsigned n) {
  if (n <= 8) {
    unsigned low = 0;
    while (low + 2 < n && x >= axis[low + 1])
      ++low;
    return low;
  }
  // A uniform-grid estimate is only a hint. Verify the actual cell before
  // using it, then fall back to exact binary lookup on nonuniform axes.
  const auto hint =
      static_cast<unsigned>(fraction(x, axis[0], axis[n - 1]) * (n - 1));
  unsigned low = std::min(hint, n - 2);
  if (!(axis[low] <= x && x < axis[low + 1])) {
    unsigned high = n - 1;
    low = 0;
    while (high - low > 1) {
      const auto mid = (low + high) / 2;
      if (x < axis[mid])
        high = mid;
      else
        low = mid;
    }
  }
  return low;
}
template <bool Periodic>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline Cell cell(double x, const double* axis, unsigned n) {
  if constexpr (Periodic) {
    x -= 2.0 * std::floor(x * .5);
    if (x >= axis[n - 1])
      return {n - 1, 0, fraction(x, axis[n - 1], 2.0), x};
  }
  if (x <= axis[0])
    return {0, 1, 0.0, axis[0]};
  if (x >= axis[n - 1])
    return {n - 2, n - 1, 1.0, axis[n - 1]};
  const unsigned low =
      n <= 3 ? (n == 3 && x >= axis[1] ? 1U : 0U) : interior_cell(x, axis, n);
  return {low, low + 1, fraction(x, axis[low], axis[low + 1]), x};
}
struct Mesh {
  unsigned kind = 0, nu = 0, nv = 0;
  const double* u = nullptr;
  const double* v = nullptr;
  template <unsigned Kind, class Read>
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  double mapped_chroma(const Cell& x, const Cell& y, double a, double b,
                       Read read) const {
    constexpr unsigned c = Kind == 1 ? 1 : 0;
    constexpr bool vertical = Kind == 1;
    const double p00 = read(x.first, y.first, c);
    const double p01 = read(x.first, y.second, c);
    const double p10 = read(x.second, y.first, c);
    const double p11 = read(x.second, y.second, c);
    const double c0 = vertical ? v[y.first] : u[x.first];
    const double c1 = vertical ? v[y.second] : u[x.second];
    const double m00 = c0 + p00;
    const double m01 = (vertical ? c1 : c0) + p01;
    const double m10 = (vertical ? c0 : c1) + p10;
    const double m11 = c1 + p11;
    const double mapped = lerp(lerp(m00, m01, y.fraction),
                               lerp(m10, m11, y.fraction), x.fraction);
    const double original = vertical ? b : a;
    const double query = vertical ? y.coordinate : x.coordinate;
    return mapped + (original - query);
  }
  template <unsigned Kind, class Read>
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((always_inline))
#endif
  std::array<double, 2> apply_kind(double a, double b, Read read) const {
    constexpr unsigned kind = Kind;
    const auto x = cell<Kind == 1>(a, u, nu);
    const auto y = cell<false>(b, v, nv);
    std::array<double, 2> result{a, b}, displacement{};
#if defined(__aarch64__)
    const auto vertex = [&](unsigned i, unsigned j) {
      const double p[2]{read(i, j, 0), read(i, j, 1)};
      return vld1q_f64(p);
    };
    const auto blend = [](float64x2_t p, float64x2_t q, double t) {
      if (t == 0.0)
        return p;
      if (t == 1.0)
        return q;
      const auto a = vreinterpretq_u64_f64(p), b = vreinterpretq_u64_f64(q);
      const auto equal =
          vorrq_u64(vceqq_u64(a, b),
                    vceqq_u64(vshlq_n_u64(vorrq_u64(a, b), 1), vdupq_n_u64(0)));
      return vbslq_f64(equal, p,
                       vaddq_f64(vmulq_n_f64(p, 1.0 - t), vmulq_n_f64(q, t)));
    };
    vst1q_f64(displacement.data(),
              blend(blend(vertex(x.first, y.first), vertex(x.first, y.second),
                          y.fraction),
                    blend(vertex(x.second, y.first), vertex(x.second, y.second),
                          y.fraction),
                    x.fraction));
#else
    for (unsigned c = 0; c < 2; ++c)
      displacement[c] = lerp(lerp(read(x.first, y.first, c),
                                  read(x.first, y.second, c), y.fraction),
                             lerp(read(x.second, y.first, c),
                                  read(x.second, y.second, c), y.fraction),
                             x.fraction);
#endif
    for (unsigned c = 0; c < 2; ++c) {
      const double d = displacement[c];
      if (d == 0)
        continue;
      const double original = c ? b : a;
      const double candidate = original + d;
      if (kind && c == (kind == 1 ? 1U : 0U) &&
          (candidate < 0 ||
           std::abs(candidate) <=
               0x1p-12 * std::max(std::abs(original), std::abs(d)))) {
        // Interpolate mapped chroma to avoid a spurious negative result from
        // cancellation in C + dC at a legal zero-chroma boundary.
        result[c] = mapped_chroma<Kind>(x, y, a, b, read);
      } else {
        result[c] = candidate;
      }
    }
    return result;
  }
  template <class Read>
  std::array<double, 2> apply(double a, double b, Read read) const {
    if (kind == 0)
      return apply_kind<0>(a, b, read);
    if (kind == 1)
      return apply_kind<1>(a, b, read);
    return apply_kind<2>(a, b, read);
  }
  template <class T, unsigned Kind, class Read>
  void span_kind(const T* source, T* destination, std::size_t n,
                 unsigned channels, const unsigned* selected, Read read) const {
    constexpr unsigned kind = Kind;
    const unsigned a = selected[kind == 0 ? 1 : kind == 1 ? 2 : 1];
    const unsigned b = selected[kind == 0 ? 2 : kind == 1 ? 1 : 0];
    if (destination != source)
      std::memcpy(destination, source, n * channels * sizeof(T));
    for (std::size_t j = 0; j < n; ++j) {
      const auto* src = source + channels * j;
      auto* dst = destination + channels * j;
      const double original_a = static_cast<double>(src[a]);
      const double original_b = static_cast<double>(src[b]);
      const auto value = apply_kind<Kind>(original_a, original_b, read);
      if (value[0] != original_a)
        dst[a] = static_cast<T>(value[0]);
      if (value[1] != original_b)
        dst[b] = static_cast<T>(value[1]);
    }
  }
  template <class T, class Read>
  void span(const T* source, T* destination, std::size_t n, unsigned channels,
            const unsigned* selected, Read read) const {
    if (kind == 0)
      span_kind<T, 0>(source, destination, n, channels, selected, read);
    else if (kind == 1)
      span_kind<T, 1>(source, destination, n, channels, selected, read);
    else
      span_kind<T, 2>(source, destination, n, channels, selected, read);
  }
};
}  // namespace ps::plugin_internal::grade::warper
