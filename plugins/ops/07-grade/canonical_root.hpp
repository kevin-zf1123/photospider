// Copyright 2026 Photospider Authors
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace ps::plugin_internal::grade::statistics {
// One per-sample sequence for every statistical/application/cache path. For a
// normal positive x, integer exponent interpolation gives an inverse-root seed
// scaled by exact 123/128 has relative error below 0.04. Four inverse Newton
// steps square that error;
// no division is required. Form x*r and r*r separately, then multiply them,
// so scale-bearing products remain normal and finite over normal binary64 x;
// a zero refinement residual is harmless. With u=2^-53, round-to-nearest FMA
// gives E_next<=2.113*E*E+4u, E_4<4.03u and final relative root error<11u.
// This is a conservative error bound, not correct rounding or a 3-ulp bound.
// Explicit fma fixes the refinement sequence independently of contraction.
// Zero, subnormal and nonfinite inputs retain the platform library behavior.
inline __attribute__((always_inline)) double canonical_root(double x) {
#if defined(__aarch64__)
  return std::cbrt(x);
#else
  std::uint64_t raw;
  std::memcpy(&raw, &x, sizeof(raw));
  const auto magnitude = raw & UINT64_C(0x7fffffffffffffff);
  const auto exponent = magnitude >> 52;
  if (exponent == 0 || exponent == 2047) {
    return std::cbrt(x);
  }
  const auto seed = (UINT64_C(1364) << 52) - magnitude / 3;
  double value, root;
  std::memcpy(&value, &magnitude, sizeof(value));
  std::memcpy(&root, &seed, sizeof(root));
  root *= 0x1.ecp-1;
  for (unsigned step = 0; step < 4; ++step) {
    const double residual = std::fma(-(value * root), root * root, 1.0);
    root = std::fma(root * 0x1.5555555555555p-2, residual, root);
  }
  return std::copysign((value * root) * root, x);
#endif
}
#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) inline std::array<double, 3>
canonical_roots_fma(const std::array<double, 3>& input) {
  double values[4]{1.0, 1.0, 1.0, 1.0}, seeds[4]{1.0, 1.0, 1.0, 1.0};
  bool normal[3];
  for (unsigned c = 0; c < 3; ++c) {
    std::uint64_t raw;
    std::memcpy(&raw, &input[c], sizeof(raw));
    const auto magnitude = raw & UINT64_C(0x7fffffffffffffff);
    const auto exponent = magnitude >> 52;
    normal[c] = exponent != 0 && exponent != 2047;
    if (normal[c]) {
      const auto seed = (UINT64_C(1364) << 52) - magnitude / 3;
      std::memcpy(values + c, &magnitude, sizeof(double));
      std::memcpy(seeds + c, &seed, sizeof(double));
    }
  }
  const auto x = _mm256_loadu_pd(values);
  auto root = _mm256_mul_pd(_mm256_loadu_pd(seeds), _mm256_set1_pd(0x1.ecp-1));
  for (unsigned step = 0; step < 4; ++step) {
    const auto residual = _mm256_fnmadd_pd(
        _mm256_mul_pd(x, root), _mm256_mul_pd(root, root), _mm256_set1_pd(1.0));
    root = _mm256_fmadd_pd(
        _mm256_mul_pd(root, _mm256_set1_pd(0x1.5555555555555p-2)), residual,
        root);
  }
  _mm256_storeu_pd(values, _mm256_mul_pd(_mm256_mul_pd(x, root), root));
  std::array<double, 3> out;
  for (unsigned c = 0; c < 3; ++c) {
    out[c] =
        normal[c] ? std::copysign(values[c], input[c]) : std::cbrt(input[c]);
  }
  return out;
}
#endif
inline std::array<double, 3> canonical_roots(std::array<double, 3> values) {
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    return canonical_roots_fma(values);
  }
#endif
  for (auto& x : values) {
    x = canonical_root(x);
  }
  return values;
}
}  // namespace ps::plugin_internal::grade::statistics
