// Copyright 2026 Photospider contributors
#pragma once
#if defined(__aarch64__)
#include <arm_neon.h>
namespace ps::plugin_internal::native_simd {
// The native profile consumes finite arithmetic operands. These exact hardware
// comparisons avoid Clang scalarizing NEON predicates under -frounding-math.
// They do not change arithmetic rounding, and do not apply to NUM profiles.
inline uint64x2_t equal(float64x2_t a, float64x2_t b) {
  uint64x2_t result;
  asm("fcmeq %0.2d, %1.2d, %2.2d" : "=w"(result) : "w"(a), "w"(b));
  return result;
}
inline uint64x2_t greater(float64x2_t a, float64x2_t b) {
  uint64x2_t result;
  asm("fcmgt %0.2d, %1.2d, %2.2d" : "=w"(result) : "w"(a), "w"(b));
  return result;
}
inline uint64x2_t greater_equal(float64x2_t a, float64x2_t b) {
  uint64x2_t result;
  asm("fcmge %0.2d, %1.2d, %2.2d" : "=w"(result) : "w"(a), "w"(b));
  return result;
}
inline uint32x4_t equal(float32x4_t a, float32x4_t b) {
  uint32x4_t result;
  asm("fcmeq %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(a), "w"(b));
  return result;
}
inline uint32x4_t greater(float32x4_t a, float32x4_t b) {
  uint32x4_t result;
  asm("fcmgt %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(a), "w"(b));
  return result;
}
inline uint32x4_t greater_equal(float32x4_t a, float32x4_t b) {
  uint32x4_t result;
  asm("fcmge %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(a), "w"(b));
  return result;
}
template <class V>
inline auto less(V a, V b) {
  return greater(b, a);
}
template <class V>
inline auto less_equal(V a, V b) {
  return greater_equal(b, a);
}
}  // namespace ps::plugin_internal::native_simd
#endif
