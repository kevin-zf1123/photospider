#pragma once

#include <cstddef>
#include <cstdint>

namespace ps::plugin_internal::numeric_ops {
// CertifiedKind values, kept independent of its definition to avoid a cycle.
inline bool trig_simd_kind(unsigned kind) {
  return kind == 2 || kind == 3 || kind == 5 || kind == 6 || kind == 8 ||
         kind == 9;
}
inline bool trig_simd_domain(unsigned kind, std::uint32_t bits) {
  const auto magnitude = bits & UINT32_C(0x7fffffff);
  if (kind == 9)
    return magnitude < UINT32_C(0x7f800000);
  const bool normalized = kind == 5 || kind == 6;
  if (!trig_simd_kind(kind) ||
      magnitude > (normalized ? UINT32_C(0x3e800000) : UINT32_C(0x3f800000)))
    return false;
  // Named quarter-turn roots retain their exact algebraic implementation.
  if ((kind == 5 || kind == 6) && magnitude == UINT32_C(0x3e800000))
    return false;
  // Sine's relative-error proof uses normal outputs. Zero is exactly handled
  // by the odd polynomial; very small nonzero sine inputs keep scalar fallback.
  return (kind != 2 && kind != 5) || !magnitude ||
         magnitude >= UINT32_C(0x03800000);  // 2^-120
}
// Finite, admitted arguments only; nearest rounding and gradual underflow.
// Same explicit FMA graph on NEON and AVX2; no overread of partial vectors.
void trig_simd_f32(unsigned kind, const float* input, float* output,
                   std::size_t count);
}  // namespace ps::plugin_internal::numeric_ops
