#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "01-numeric/exact_ratio.hpp"
#include "02-format-color/alpha_simd.hpp"

namespace ps::plugin_internal::alpha_ops {
inline std::uint64_t load_bits(const void* source, bool narrow) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, source, narrow ? 4 : 8);
  return bits;
}
/** Caller lends a live RN-even/gradual-underflow environment, charges bounded
 * work and polls between blocks. Buffers contain exactly count authorized
 * samples. SIMD uses division, NOT approximate reciprocal multiplication.
 * No alignment or padding is assumed. All nonfinite raw lanes are repaired
 * using the NUM first-operand NaN table, independently of ISA NaN selection. */
inline Status math_span_slow(
    const std::uint8_t* x, const std::uint8_t* a, std::uint8_t* y,
    std::uint64_t count, bool narrow, Action action, bool raw, bool reference,
    const plugin_internal::FloatingEnvironment& environment,
    const core_internal::WorkConsumer& consume,
    numeric_ops::RatioWorkspace* exact) {
  const std::size_t width = narrow ? 4 : 8;
  const auto inf = narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
  const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
  if (action == Action::Set) {
    std::memcpy(y, x, count * width);
    return {};
  }
  const bool divide = action == Action::Unassociate;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto u = load_bits(x + i * width, narrow);
    const auto v = load_bits(a + i * width, narrow);
    std::uint64_t result = 0;
    const auto um = u & ~sign, vm = v & ~sign;
    const auto s = (u ^ v) & sign;
    if (!raw && vm == 0) {
      result = 0;  // Semantic canonical +0, including negative alpha zero.
    } else if (raw && (um > inf || vm > inf)) {
      result = (um > inf ? u : v) | quiet;
    } else if (raw && (!divide ? ((!um && vm == inf) || (!vm && um == inf))
                               : ((!um && !vm) || (um == inf && vm == inf)))) {
      result = inf | quiet;
    } else if (raw &&
               (!divide ? (um == inf || vm == inf) : (um == inf || !vm))) {
      result = inf | s;
    } else if (raw && (!divide ? (!um || !vm) : (!um || vm == inf))) {
      result = s;
    } else if (!um) {
      result = s;
    } else if (reference) {
      // A genuinely independent finite arithmetic path: fixed-capacity exact
      // integers followed by one ties-to-even rounding, never host mul/div.
      // Binary64 products need at most 4196 bits; NUM's 68-word workspace
      // also covers denominator alignment in division. Its work and cancel
      // checks are forwarded, including ResourceExhausted/Cancelled failures.
      const auto left = numeric_ops::BinaryParts::decode(u, narrow);
      const auto right = numeric_ops::BinaryParts::decode(v, narrow);
      exact->numerator.words.fill(0);
      exact->denominator.words.fill(0);
      exact->negative = left.negative != right.negative;
      if (divide) {
        exact->numerator.set(left, 1074);
        exact->denominator.set(right, 1074);
      } else {
        exact->numerator.set_product(left, right);
        exact->denominator.words[0] = 1;
      }
      auto answer = exact->round(narrow, consume, divide ? 0 : -2148);
      if (!answer.ok()) {
        return answer.status();
      }
      result = answer.value();
    } else if (narrow) {
      float left, right, value;
      std::memcpy(&left, &u, 4);
      std::memcpy(&right, &v, 4);
      value = divide ? left / right : left * right;
      std::memcpy(&result, &value, 4);
    } else {
      double left, right, value;
      std::memcpy(&left, &u, 8);
      std::memcpy(&right, &v, 8);
      value = divide ? left / right : left * right;
      std::memcpy(&result, &value, 8);
    }
    std::memcpy(y + i * width, &result, width);
  }
  return {};
}
// The caller supplies valid semantic operands. Raw exceptional spans replay
// through the bit table so ISA NaN selection never defines the result.
inline Status math_span(const std::uint8_t* x, const std::uint8_t* a,
                        std::uint8_t* y, std::uint64_t count, bool narrow,
                        Action action, bool raw, bool simd, bool reference,
                        const plugin_internal::FloatingEnvironment& environment,
                        const core_internal::WorkConsumer& consume,
                        numeric_ops::RatioWorkspace* exact) {
  if (action == Action::Set) {
    std::memcpy(y, x, count * (narrow ? 4 : 8));
    return Status::success();
  }
  if (!count) {
    return {};
  }
  if (!reference && environment.active() &&
      finite_span(x, a, y, count, narrow, simd, raw, action)) {
    return {};
  }
  return math_span_slow(x, a, y, count, narrow, action, raw, reference,
                        environment, consume, exact);
}
}  // namespace ps::plugin_internal::alpha_ops
