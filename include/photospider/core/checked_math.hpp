#pragma once

#include <cstdint>
#include <limits>

namespace ps::core_internal {
// Predicates never evaluate overflowing arithmetic. Limits describe the
// largest accepted result, including zero products with very large factors.
constexpr bool can_add(
    std::uint64_t a, std::uint64_t b,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  return a <= limit && b <= limit - a;
}
constexpr bool can_multiply(
    std::uint64_t a, std::uint64_t b,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  return a == 0 || b <= limit / a;
}
constexpr bool can_multiply_add(
    std::uint64_t a, std::uint64_t b, std::uint64_t base,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  return base <= limit && can_multiply(a, b, limit - base);
}
constexpr std::uint64_t saturating_multiply(
    std::uint64_t a, std::uint64_t b,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  return can_multiply(a, b, limit) ? a * b : limit;
}
// out must be nonnull. Failure leaves it unchanged; input/output aliasing is
// supported. Callers retain their own error category and diagnostic policy.
inline bool checked_add(
    std::uint64_t a, std::uint64_t b, std::uint64_t* out,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  if (!can_add(a, b, limit))
    return false;
  *out = a + b;
  return true;
}
inline bool checked_multiply(
    std::uint64_t a, std::uint64_t b, std::uint64_t* out,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  if (!can_multiply(a, b, limit))
    return false;
  *out = a * b;
  return true;
}
inline bool checked_multiply_add(
    std::uint64_t a, std::uint64_t b, std::uint64_t base, std::uint64_t* out,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  if (!can_multiply_add(a, b, base, limit))
    return false;
  *out = base + a * b;
  return true;
}
inline bool checked_align_up(
    std::uint64_t value, std::uint64_t alignment, std::uint64_t* out,
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
  if (!alignment)
    return false;
  const auto remainder = value % alignment;
  return checked_add(value, remainder ? alignment - remainder : 0, out, limit);
}
}  // namespace ps::core_internal
