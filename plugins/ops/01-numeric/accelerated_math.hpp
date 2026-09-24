#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#include "01-numeric/exp_simd.hpp"
#include "data/input_validation.hpp"

extern "C" void photospider_sleef_evaluate(unsigned kind, const double* a,
                                           const double* b, double* output,
                                           std::size_t count);

namespace ps::plugin_internal::numeric_ops {
inline double numeric_double(std::uint64_t bits, bool narrow = false) {
  if (narrow) {
    const auto word = static_cast<std::uint32_t>(bits);
    float value = 0;
    std::memcpy(&value, &word, 4);
    return value;
  }
  double value = 0;
  std::memcpy(&value, &bits, 8);
  return value;
}
inline std::uint64_t numeric_bits(double value, bool narrow = false) {
  std::uint64_t bits = 0;
  if (narrow) {
    const float result = static_cast<float>(value);
    std::memcpy(&bits, &result, 4);
  } else {
    std::memcpy(&bits, &value, 8);
  }
  return bits;
}
inline double numeric_down(double value) {
  return std::nextafter(value, -std::numeric_limits<double>::infinity());
}
inline double numeric_up(double value) {
  return std::nextafter(value, std::numeric_limits<double>::infinity());
}
// Outward enclosures, evaluated in the caller's scoped nearest/gradual mode.
// Basic arithmetic encloses the exact real formula and its RN64 result.
struct FastInterval final {
  double low = 0, high = 0;
  static FastInterval point(double value) { return {value, value}; }
  bool finite() const {
    return std::isfinite(low) && std::isfinite(high) && low <= high;
  }
  bool nonzero() const { return low > 0 || high < 0; }
  FastInterval operator-() const { return {-high, -low}; }
  FastInterval operator+(FastInterval b) const {
    return {numeric_down(low + b.low), numeric_up(high + b.high)};
  }
  FastInterval operator-(FastInterval b) const { return *this + (-b); }
  FastInterval operator*(FastInterval b) const {
    const double products[] = {low * b.low, low * b.high, high * b.low,
                               high * b.high};
    return {numeric_down(*std::min_element(products, products + 4)),
            numeric_up(*std::max_element(products, products + 4))};
  }
  FastInterval operator/(FastInterval b) const {
    if (!b.nonzero())
      return {-INFINITY, INFINITY};
    return *this *
           FastInterval{numeric_down(1 / b.high), numeric_up(1 / b.low)};
  }
  // Includes destination rounding. A 2-ULP32 absolute guard is deliberately
  // tighter than the public four ordered-step allowance at binade boundaries.
  std::optional<std::uint64_t> accepted(double candidate, bool narrow) const {
    constexpr double maximum = 0x1.fffffep127;
    if (!finite() || !nonzero())
      return {};
    const double nearest = std::min(std::abs(low), std::abs(high));
    if (nearest < 0x1p-126 || std::max(std::abs(low), std::abs(high)) > maximum)
      return {};
    const auto bits = numeric_bits(candidate, narrow);
    const double rounded = numeric_double(bits, narrow);
    const double tolerance = std::ldexp(1.0, std::ilogb(nearest) - 22);
    const double error =
        numeric_up(std::max(std::abs(rounded - low), std::abs(rounded - high)));
    if (std::isfinite(rounded) && error <= tolerance)
      return bits;
    return {};
  }
};
inline bool accelerated_math_available() {
#if defined(__APPLE__) && defined(__aarch64__)
  return true;
#elif defined(__x86_64__)
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}
// Restrict to verified, well-conditioned reduction domains. Broader legal
// inputs retain the certified implementation. Numbers are CertifiedKind values.
inline bool accelerated_math_domain(unsigned kind, double a, double b) {
  if (!accelerated_math_available() || !std::isfinite(a) || !std::isfinite(b))
    return false;
  switch (kind) {
    case 1:
      return a >= 0x1p-100 && a <= 0x1p100;
    case 2:
    case 3:
    case 4:
      return std::abs(a) <= 1;
    case 10:
      return a >= 0x1p-16 && a <= 0x1p16 && std::abs(b) <= 16;
    case 11:
      return a != 0 && b != 0 && std::abs(a) >= 0x1p-100 &&
             std::abs(b) >= 0x1p-100 && std::abs(a) <= 0x1p100 &&
             std::abs(b) <= 0x1p100;
    default:
      return false;
  }
}
// SLEEF u10 bounds the real function by one binary64 ULP. Four adjacent
// steps enclose that error, binade asymmetry, and the strict RN64 reference.
inline FastInterval accelerated_math_enclosure(double result) {
  double low = result, high = result;
  for (unsigned step = 0; step < 4; ++step) {
    low = numeric_down(low);
    high = numeric_up(high);
  }
  return {low, high};
}
inline std::optional<std::uint64_t> accelerated_math(
    unsigned kind, std::uint64_t a, std::uint64_t b, bool narrow,
    bool normalized_environment = false) {
  std::optional<input_internal::Float32Environment> environment;
  if (!normalized_environment)
    environment.emplace();
  if (!normalized_environment && !environment->active())
    return {};
  const double x = numeric_double(a, narrow), y = numeric_double(b, narrow);
  if (kind == 0) {
    // IQK is certified only for binary32 arguments. Float64, expression
    // intervals and unsupported inputs retain the strict exp implementation.
    if (!narrow || !accelerated_math_available() || !std::isfinite(x) ||
        x < -80 || x > 80)
      return {};
    const float input = static_cast<float>(x);
    float output = 0;
    exp_simd_f32(&input, &output, 1);
    return numeric_bits(output, true);
  }
  if (!accelerated_math_domain(kind, x, y))
    return {};
  double result = 0;
  photospider_sleef_evaluate(kind, &x, &y, &result, 1);
  return accelerated_math_enclosure(result).accepted(result, narrow);
}
}  // namespace ps::plugin_internal::numeric_ops
