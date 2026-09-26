#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::alpha_ops {
enum class Action { Associate, Unassociate, Set };
inline constexpr std::uint64_t kAlphaFallback = UINT64_MAX;
inline bool simd_available() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2");
#elif defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

// Hot finite path. No ratio workspace, erased callback, dynamic dtype branch,
// or NaN repair table is visited here. A rejected span is replayed in full by
// the independent slow path; domain errors therefore still precede overflow.
// Output is disjoint from the two input byte buffers; x and a may alias.
// Each buffer contains exactly count authorized samples.
template <bool Narrow, Action Op, bool Raw>
inline std::uint64_t scalar_checked(const std::uint8_t* x,
                                    const std::uint8_t* a, std::uint8_t* y,
                                    std::uint64_t count) {
  using UInt = std::conditional_t<Narrow, std::uint32_t, std::uint64_t>;
  using Float = std::conditional_t<Narrow, float, double>;
  constexpr UInt inf =
      Narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  constexpr UInt sign = UINT64_C(1) << (Narrow ? 31 : 63);
  constexpr UInt one =
      Narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
  for (std::uint64_t i = 0; i < count; ++i) {
    UInt u, v;
    std::memcpy(&u, x + i * sizeof(UInt), sizeof(UInt));
    std::memcpy(&v, a + i * sizeof(UInt), sizeof(UInt));
    const auto um = u & ~sign, vm = v & ~sign;
    if constexpr (Raw) {
      if (um >= inf || vm >= inf || (Op == Action::Unassociate && vm == 0)) {
        return kAlphaFallback;
      }
    } else {
      if (um >= inf || !(v <= one || v == sign) ||
          (Op == Action::Unassociate && vm == 0 && um != 0)) {
        return kAlphaFallback;
      }
    }
    UInt result = u;
    if constexpr (Op != Action::Set) {
      if (!Raw && vm == 0) {
        result = 0;
      } else {
        Float left, right, value;
        std::memcpy(&left, &u, sizeof(Float));
        std::memcpy(&right, &v, sizeof(Float));
        if constexpr (Op == Action::Unassociate) {
          value = left / right;
        } else {
          value = left * right;
        }
        std::memcpy(&result, &value, sizeof(UInt));
        if constexpr (!Raw) {
          if ((result & inf) == inf) {
            return kAlphaFallback;
          }
        }
      }
    }
    std::memcpy(y + i * sizeof(UInt), &result, sizeof(UInt));
  }
  return count;
}

// Validate in integer lanes before doing any floating arithmetic: signaling
// NaNs never reach FP comparisons. Semantic zero alpha is canonicalized to +0;
// division uses a safe 1 denominator on zero lanes, never speculative 0/0.
// Raw exceptional lanes replay through the NUM first-operand payload table.
// No reciprocal estimates, fast-math, alignment or padding assumptions.
#if defined(__x86_64__)
template <bool Narrow, Action Op, bool Raw>
__attribute__((target("avx2"))) inline std::uint64_t vector_checked(
    const std::uint8_t* x, const std::uint8_t* a, std::uint8_t* y,
    std::uint64_t count) {
  std::uint64_t i = 0;
  const auto z = _mm256_setzero_si256();
  if constexpr (Narrow) {
    const auto abs_mask = _mm256_set1_epi32(INT32_MAX);
    const auto inf = _mm256_set1_epi32(0x7f800000);
    const auto one = _mm256_set1_epi32(0x3f800000);
    for (; i + 8 <= count; i += 8) {
      const auto u =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i * 4));
      const auto v =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 4));
      const auto um = _mm256_and_si256(u, abs_mask);
      const auto vm = _mm256_and_si256(v, abs_mask);
      const auto az = _mm256_cmpeq_epi32(vm, z);
      auto bad = _mm256_cmpeq_epi32(_mm256_and_si256(u, inf), inf);
      if constexpr (Raw) {
        bad = _mm256_or_si256(
            bad, _mm256_cmpeq_epi32(_mm256_and_si256(v, inf), inf));
        if constexpr (Op == Action::Unassociate) {
          bad = _mm256_or_si256(bad, az);
        }
      } else {
        bad = _mm256_or_si256(bad, _mm256_cmpgt_epi32(vm, one));
        bad = _mm256_or_si256(
            bad, _mm256_andnot_si256(az, _mm256_cmpgt_epi32(z, v)));
        if constexpr (Op == Action::Unassociate) {
          bad = _mm256_or_si256(
              bad, _mm256_andnot_si256(_mm256_cmpeq_epi32(um, z), az));
        }
      }
      if (!_mm256_testz_si256(bad, bad)) {
        return kAlphaFallback;
      }
      auto result = u;
      if constexpr (Op != Action::Set) {
        const auto left = _mm256_castsi256_ps(u);
        auto right = _mm256_castsi256_ps(v);
        if constexpr (!Raw && Op == Action::Unassociate) {
          right = _mm256_blendv_ps(right, _mm256_castsi256_ps(one),
                                   _mm256_castsi256_ps(az));
        }
        if constexpr (Op == Action::Unassociate) {
          result = _mm256_castps_si256(_mm256_div_ps(left, right));
        } else {
          result = _mm256_castps_si256(_mm256_mul_ps(left, right));
        }
        if constexpr (!Raw) {
          result = _mm256_andnot_si256(az, result);
          bad = _mm256_cmpeq_epi32(_mm256_and_si256(result, inf), inf);
          if (!_mm256_testz_si256(bad, bad)) {
            return kAlphaFallback;
          }
        }
      }
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(y + i * 4), result);
    }
  } else {
    const auto abs_mask = _mm256_set1_epi64x(INT64_MAX);
    const auto inf = _mm256_set1_epi64x(0x7ff0000000000000);
    const auto one = _mm256_set1_epi64x(0x3ff0000000000000);
    for (; i + 4 <= count; i += 4) {
      const auto u =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i * 8));
      const auto v =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 8));
      const auto um = _mm256_and_si256(u, abs_mask);
      const auto vm = _mm256_and_si256(v, abs_mask);
      const auto az = _mm256_cmpeq_epi64(vm, z);
      auto bad = _mm256_cmpeq_epi64(_mm256_and_si256(u, inf), inf);
      if constexpr (Raw) {
        bad = _mm256_or_si256(
            bad, _mm256_cmpeq_epi64(_mm256_and_si256(v, inf), inf));
        if constexpr (Op == Action::Unassociate) {
          bad = _mm256_or_si256(bad, az);
        }
      } else {
        bad = _mm256_or_si256(bad, _mm256_cmpgt_epi64(vm, one));
        bad = _mm256_or_si256(
            bad, _mm256_andnot_si256(az, _mm256_cmpgt_epi64(z, v)));
        if constexpr (Op == Action::Unassociate) {
          bad = _mm256_or_si256(
              bad, _mm256_andnot_si256(_mm256_cmpeq_epi64(um, z), az));
        }
      }
      if (!_mm256_testz_si256(bad, bad)) {
        return kAlphaFallback;
      }
      auto result = u;
      if constexpr (Op != Action::Set) {
        const auto left = _mm256_castsi256_pd(u);
        auto right = _mm256_castsi256_pd(v);
        if constexpr (!Raw && Op == Action::Unassociate) {
          right = _mm256_blendv_pd(right, _mm256_castsi256_pd(one),
                                   _mm256_castsi256_pd(az));
        }
        if constexpr (Op == Action::Unassociate) {
          result = _mm256_castpd_si256(_mm256_div_pd(left, right));
        } else {
          result = _mm256_castpd_si256(_mm256_mul_pd(left, right));
        }
        if constexpr (!Raw) {
          result = _mm256_andnot_si256(az, result);
          bad = _mm256_cmpeq_epi64(_mm256_and_si256(result, inf), inf);
          if (!_mm256_testz_si256(bad, bad)) {
            return kAlphaFallback;
          }
        }
      }
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(y + i * 8), result);
    }
  }
  return i;
}
#elif defined(__aarch64__)
template <bool Narrow, Action Op, bool Raw>
inline std::uint64_t vector_checked(const std::uint8_t* x,
                                    const std::uint8_t* a, std::uint8_t* y,
                                    std::uint64_t count) {
  std::uint64_t i = 0;
  if constexpr (Narrow) {
    const auto z = vdupq_n_u32(0);
    const auto abs_mask = vdupq_n_u32(0x7fffffffU);
    const auto inf = vdupq_n_u32(0x7f800000U);
    const auto one = vdupq_n_u32(0x3f800000U);
    const auto sign = vdupq_n_u32(0x80000000U);
    for (; i + 4 <= count; i += 4) {
      const auto u = vreinterpretq_u32_u8(vld1q_u8(x + i * 4));
      const auto v = vreinterpretq_u32_u8(vld1q_u8(a + i * 4));
      const auto um = vandq_u32(u, abs_mask);
      const auto vm = vandq_u32(v, abs_mask);
      const auto az = vceqq_u32(vm, z);
      auto bad = vceqq_u32(vandq_u32(u, inf), inf);
      if constexpr (Raw) {
        bad = vorrq_u32(bad, vceqq_u32(vandq_u32(v, inf), inf));
        if constexpr (Op == Action::Unassociate) {
          bad = vorrq_u32(bad, az);
        }
      } else {
        // Unsigned bit order compares positive finite values exactly; the
        // only permitted sign-bit pattern is negative zero.
        bad = vorrq_u32(bad, vbicq_u32(vcgtq_u32(v, one), vceqq_u32(v, sign)));
        if constexpr (Op == Action::Unassociate) {
          bad = vorrq_u32(bad, vbicq_u32(az, vceqq_u32(um, z)));
        }
      }
      if (vmaxvq_u32(bad)) {
        return kAlphaFallback;
      }
      auto result = u;
      if constexpr (Op != Action::Set) {
        const auto left = vreinterpretq_f32_u32(u);
        auto right = vreinterpretq_f32_u32(v);
        if constexpr (!Raw && Op == Action::Unassociate) {
          right = vreinterpretq_f32_u32(vbslq_u32(az, one, v));
        }
        if constexpr (Op == Action::Unassociate) {
          result = vreinterpretq_u32_f32(vdivq_f32(left, right));
        } else {
          result = vreinterpretq_u32_f32(vmulq_f32(left, right));
        }
        if constexpr (!Raw) {
          result = vbicq_u32(result, az);
          bad = vceqq_u32(vandq_u32(result, inf), inf);
          if (vmaxvq_u32(bad)) {
            return kAlphaFallback;
          }
        }
      }
      vst1q_u8(y + i * 4, vreinterpretq_u8_u32(result));
    }
  } else {
    const auto z = vdupq_n_u64(0);
    const auto abs_mask = vdupq_n_u64(UINT64_C(0x7fffffffffffffff));
    const auto inf = vdupq_n_u64(UINT64_C(0x7ff0000000000000));
    const auto one = vdupq_n_u64(UINT64_C(0x3ff0000000000000));
    const auto sign = vdupq_n_u64(UINT64_C(0x8000000000000000));
    for (; i + 2 <= count; i += 2) {
      const auto u = vreinterpretq_u64_u8(vld1q_u8(x + i * 8));
      const auto v = vreinterpretq_u64_u8(vld1q_u8(a + i * 8));
      const auto um = vandq_u64(u, abs_mask);
      const auto vm = vandq_u64(v, abs_mask);
      const auto az = vceqq_u64(vm, z);
      auto bad = vceqq_u64(vandq_u64(u, inf), inf);
      if constexpr (Raw) {
        bad = vorrq_u64(bad, vceqq_u64(vandq_u64(v, inf), inf));
        if constexpr (Op == Action::Unassociate) {
          bad = vorrq_u64(bad, az);
        }
      } else {
        // Unsigned bit order compares positive finite values exactly; the
        // only permitted sign-bit pattern is negative zero.
        bad = vorrq_u64(bad, vbicq_u64(vcgtq_u64(v, one), vceqq_u64(v, sign)));
        if constexpr (Op == Action::Unassociate) {
          bad = vorrq_u64(bad, vbicq_u64(az, vceqq_u64(um, z)));
        }
      }
      if (vmaxvq_u32(vreinterpretq_u32_u64(bad))) {
        return kAlphaFallback;
      }
      auto result = u;
      if constexpr (Op != Action::Set) {
        const auto left = vreinterpretq_f64_u64(u);
        auto right = vreinterpretq_f64_u64(v);
        if constexpr (!Raw && Op == Action::Unassociate) {
          right = vreinterpretq_f64_u64(vbslq_u64(az, one, v));
        }
        if constexpr (Op == Action::Unassociate) {
          result = vreinterpretq_u64_f64(vdivq_f64(left, right));
        } else {
          result = vreinterpretq_u64_f64(vmulq_f64(left, right));
        }
        if constexpr (!Raw) {
          result = vbicq_u64(result, az);
          bad = vceqq_u64(vandq_u64(result, inf), inf);
          if (vmaxvq_u32(vreinterpretq_u32_u64(bad))) {
            return kAlphaFallback;
          }
        }
      }
      vst1q_u8(y + i * 8, vreinterpretq_u8_u64(result));
    }
  }
  return i;
}
#else
template <bool Narrow, Action Op, bool Raw>
inline std::uint64_t vector_checked(const std::uint8_t*, const std::uint8_t*,
                                    std::uint8_t*, std::uint64_t) {
  return 0;
}
#endif

template <bool Narrow, Action Op, bool Raw>
inline bool finite_checked(const std::uint8_t* x, const std::uint8_t* a,
                           std::uint8_t* y, std::uint64_t count, bool simd) {
  const auto done = simd ? vector_checked<Narrow, Op, Raw>(x, a, y, count) : 0;
  if (done == kAlphaFallback) {
    return false;
  }
  constexpr std::size_t width = Narrow ? 4 : 8;
  return scalar_checked<Narrow, Op, Raw>(x + done * width, a + done * width,
                                         y + done * width,
                                         count - done) != kAlphaFallback;
}
template <bool Narrow, bool Raw>
inline bool finite_checked(const std::uint8_t* x, const std::uint8_t* a,
                           std::uint8_t* y, std::uint64_t count, bool simd,
                           Action action) {
  switch (action) {
    case Action::Associate:
      return finite_checked<Narrow, Action::Associate, Raw>(x, a, y, count,
                                                            simd);
    case Action::Unassociate:
      return finite_checked<Narrow, Action::Unassociate, Raw>(x, a, y, count,
                                                              simd);
    case Action::Set:
      return finite_checked<Narrow, Action::Set, Raw>(x, a, y, count, simd);
  }
  return false;
}
inline bool finite_checked(const std::uint8_t* x, const std::uint8_t* a,
                           std::uint8_t* y, std::uint64_t count, bool narrow,
                           bool simd, bool raw, Action action) {
  if (narrow) {
    return raw ? finite_checked<true, true>(x, a, y, count, simd, action)
               : finite_checked<true, false>(x, a, y, count, simd, action);
  }
  return raw ? finite_checked<false, true>(x, a, y, count, simd, action)
             : finite_checked<false, false>(x, a, y, count, simd, action);
}
}  // namespace ps::plugin_internal::alpha_ops
