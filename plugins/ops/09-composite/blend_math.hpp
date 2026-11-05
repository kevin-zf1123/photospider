// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "01-numeric/fast_native_simd.hpp"
#if defined(PHOTOSPIDER_HAS_BLEND_SME)
#include <sys/sysctl.h>

#include "09-composite/blend_sme.hpp"
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::blend {
/** Independent BLN color functions; values are native Float32 or Float64. */
enum class Kind {
  Normal,
  Add,
  Subtract,
  Divide,
  Multiply,
  MultiplyUnbounded,
  Darken,
  Lighten,
  Screen,
  ScreenUnbounded,
  Overlay,
  HardLight,
  SoftLight,
  ColorDodge,
  ColorBurn,
  LinearDodge,
  LinearBurn,
  LinearLight,
  VividLight,
  PinLight,
  Difference,
  Exclusion,
  HardMixVivid,
  HardMixSum
};

template <class T>
struct Scalar final {
  using Value = T;
  template <unsigned Quarter>
  static T constant() {
    constexpr T value = static_cast<T>(Quarter) / static_cast<T>(4);
    return value;
  }
  static T add(T a, T b) { return a + b; }
  static T sub(T a, T b) { return a - b; }
  static T mul(T a, T b) { return a * b; }
  static T div(T a, T b) { return a / b; }
  static T absolute(T a) { return std::abs(a); }
  static T root(T a) { return std::sqrt(a); }
  static bool le(T a, T b) { return a <= b; }
  static bool eq(T a, T b) { return a == b; }
  static T choose(bool mask, T a, T b) { return mask ? a : b; }
};

/**
 * One arithmetic graph for scalar tails, strided samples and SIMD packets.
 * Comparisons retain stored-input endpoint priority and background tie bits.
 * Reassociation is deliberately local to the bounded Screen expression.
 */
template <Kind K, class O>
inline typename O::Value evaluate(typename O::Value b, typename O::Value s) {
  if constexpr (K == Kind::Normal) {
    return s;
  } else if constexpr (K == Kind::Add) {
    return O::add(b, s);
  } else if constexpr (K == Kind::Subtract) {
    return O::sub(b, s);
  } else if constexpr (K == Kind::Divide) {
    return O::div(b, s);
  } else if constexpr (K == Kind::Multiply || K == Kind::MultiplyUnbounded) {
    return O::mul(b, s);
  } else if constexpr (K == Kind::Darken) {
    return O::choose(O::le(b, s), b, s);
  } else if constexpr (K == Kind::Lighten) {
    return O::choose(O::le(s, b), b, s);
  } else if constexpr (K == Kind::Screen) {
    return O::add(b, O::mul(O::sub(O::template constant<4>(), b), s));
  } else if constexpr (K == Kind::ScreenUnbounded) {
    return O::sub(O::add(b, s), O::mul(b, s));
  } else if constexpr (K == Kind::Overlay || K == Kind::HardLight) {
    const auto one = O::template constant<4>(), two = O::template constant<8>();
    const auto low = O::mul(O::mul(two, b), s);
    const auto high =
        O::sub(one, O::mul(O::mul(two, O::sub(one, b)), O::sub(one, s)));
    return O::choose(
        O::le(K == Kind::Overlay ? b : s, O::template constant<2>()), low,
        high);
  } else if constexpr (K == Kind::SoftLight) {
    const auto one = O::template constant<4>(), two = O::template constant<8>();
    const auto twice = O::mul(two, s);
    const auto low =
        O::sub(b, O::mul(O::mul(O::sub(one, twice), b), O::sub(one, b)));
    const auto polynomial =
        O::mul(O::add(O::mul(O::sub(O::mul(O::template constant<64>(), b),
                                    O::template constant<48>()),
                             b),
                      O::template constant<16>()),
               b);
    const auto d =
        O::choose(O::le(b, O::template constant<1>()), polynomial, O::root(b));
    const auto high = O::add(b, O::mul(O::sub(twice, one), O::sub(d, b)));
    return O::choose(O::le(s, O::template constant<2>()), low, high);
  } else if constexpr (K == Kind::ColorDodge) {
    const auto zero = O::template constant<0>(),
               one = O::template constant<4>();
    const auto endpoint = O::eq(s, one);
    // Inactive endpoint lanes use a nonzero denominator, including B=0,S=1.
    const auto denominator = O::choose(endpoint, one, O::sub(one, s));
    const auto q = O::div(b, denominator);
    const auto bounded = O::choose(O::le(one, q), one, q);
    return O::choose(O::eq(b, zero), zero, O::choose(endpoint, one, bounded));
  } else if constexpr (K == Kind::ColorBurn) {
    const auto zero = O::template constant<0>(),
               one = O::template constant<4>();
    const auto endpoint = O::eq(s, zero);
    const auto q = O::div(O::sub(one, b), O::choose(endpoint, one, s));
    const auto bounded = O::sub(one, O::choose(O::le(one, q), one, q));
    return O::choose(O::eq(b, one), one, O::choose(endpoint, zero, bounded));
  } else if constexpr (K == Kind::Difference) {
    return O::absolute(O::sub(b, s));
  } else if constexpr (K == Kind::Exclusion) {
    return O::sub(O::add(b, s),
                  O::mul(O::mul(O::template constant<8>(), b), s));
  } else if constexpr (K == Kind::LinearDodge) {
    const auto sum = O::add(b, s), one = O::template constant<4>();
    return O::choose(O::le(one, sum), one, sum);
  } else if constexpr (K == Kind::LinearBurn) {
    const auto value = O::sub(O::add(b, s), O::template constant<4>());
    const auto zero = O::template constant<0>();
    return O::choose(O::le(value, zero), zero, value);
  } else if constexpr (K == Kind::LinearLight) {
    const auto zero = O::template constant<0>(),
               one = O::template constant<4>();
    const auto value =
        O::add(b, O::sub(O::mul(O::template constant<8>(), s), one));
    return O::choose(O::le(value, zero), zero,
                     O::choose(O::le(one, value), one, value));
  } else if constexpr (K == Kind::PinLight) {
    const auto twice = O::mul(O::template constant<8>(), s);
    const auto high = O::sub(twice, O::template constant<4>());
    return O::choose(O::le(s, O::template constant<2>()),
                     O::choose(O::le(b, twice), b, twice),
                     O::choose(O::le(high, b), b, high));
  } else if constexpr (K == Kind::VividLight) {
    const auto zero = O::template constant<0>(),
               one = O::template constant<4>();
    const auto low = O::le(s, O::template constant<2>());
    const auto numerator = O::choose(low, O::sub(one, b), b);
    const auto denominator =
        O::mul(O::template constant<8>(), O::choose(low, s, O::sub(one, s)));
    const auto endpoint = O::eq(denominator, zero);
    const auto q = O::div(numerator, O::choose(endpoint, one, denominator));
    const auto bounded = O::choose(O::le(one, q), one, q);
    const auto value = O::choose(low, O::sub(one, bounded), bounded);
    const auto end_value = O::choose(low, zero, one);
    const auto result = O::choose(endpoint, end_value, value);
    return O::choose(O::eq(numerator, zero), O::choose(low, one, zero), result);
  } else {
    static_assert(K == Kind::HardMixVivid || K == Kind::HardMixSum);
    const auto one = O::template constant<4>(),
               zero = O::template constant<0>();
    const auto small = O::choose(O::le(b, s), b, s);
    const auto large = O::choose(O::le(b, s), s, b);
    // For large >= 1/2, Sterbenz makes 1-large exact. A rounded B+S
    // cannot decide the specified real threshold at its adjacent inputs.
    const auto result = O::choose(O::le(O::sub(one, large), small), one, zero);
    if constexpr (K == Kind::HardMixVivid)
      return O::choose(O::eq(b, zero), zero, result);
    return result;
  }
}

#if defined(__aarch64__)
template <class T>
struct Neon;
#define PS_BLEND_NEON(T, SUFFIX, VECTOR, MASK, LANES)                          \
  template <>                                                                  \
  struct Neon<T> final {                                                       \
    using Value = VECTOR;                                                      \
    static constexpr unsigned lanes = LANES;                                   \
    template <unsigned Quarter>                                                \
    static Value constant() {                                                  \
      constexpr T value = static_cast<T>(Quarter) / static_cast<T>(4);         \
      return vdupq_n_##SUFFIX(value);                                          \
    }                                                                          \
    static Value load(const T* p) { return vld1q_##SUFFIX(p); }                \
    static void save(T* p, Value x) { vst1q_##SUFFIX(p, x); }                  \
    static Value add(Value a, Value b) { return vaddq_##SUFFIX(a, b); }        \
    static Value sub(Value a, Value b) { return vsubq_##SUFFIX(a, b); }        \
    static Value mul(Value a, Value b) { return vmulq_##SUFFIX(a, b); }        \
    static Value div(Value a, Value b) { return vdivq_##SUFFIX(a, b); }        \
    static Value absolute(Value a) { return vabsq_##SUFFIX(a); }               \
    static Value root(Value a) { return vsqrtq_##SUFFIX(a); }                  \
    static MASK le(Value a, Value b) { return native_simd::less_equal(a, b); } \
    static MASK eq(Value a, Value b) { return native_simd::equal(a, b); }      \
    static Value choose(MASK m, Value a, Value b) {                            \
      Value result;                                                            \
      std::memcpy(&result, &m, sizeof(result));                                \
      asm("bsl %0.16b, %1.16b, %2.16b" : "+w"(result) : "w"(a), "w"(b));       \
      return result;                                                           \
    }                                                                          \
  }
PS_BLEND_NEON(float, f32, float32x4_t, uint32x4_t, 4);
PS_BLEND_NEON(double, f64, float64x2_t, uint64x2_t, 2);
#undef PS_BLEND_NEON

template <Kind K, class T, bool Streaming>
inline std::size_t packets(const T* background, const T* source, T* output,
                           std::size_t count) {
  using O = Neon<T>;
  constexpr unsigned kLanes = O::lanes;
  constexpr unsigned kPackets = 8;
  std::size_t i = 0;
  for (; i + kPackets * kLanes <= count; i += kPackets * kLanes) {
    typename O::Value values[kPackets];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < kPackets; j += 2) {
      const auto k = i + j * kLanes;
      typename O::Value b0, b1, s0, s1;
      if constexpr (Streaming) {
        // AArch64's non-temporal hint changes cache policy, never ownership.
        asm("ldnp %q0, %q1, [%2]"
            : "=w"(b0), "=w"(b1)
            : "r"(background + k)
            : "memory");
        asm("ldnp %q0, %q1, [%2]"
            : "=w"(s0), "=w"(s1)
            : "r"(source + k)
            : "memory");
      } else {
        b0 = O::load(background + k);
        b1 = O::load(background + k + kLanes);
        s0 = O::load(source + k);
        s1 = O::load(source + k + kLanes);
      }
      values[j] = evaluate<K, O>(b0, s0);
      values[j + 1] = evaluate<K, O>(b1, s1);
    }
    // Independent loads precede stores even when pages share low address bits.
    asm volatile("" ::: "memory");
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (unsigned j = 0; j < kPackets; ++j)
      O::save(output + i + j * kLanes, values[j]);
  }
  for (; i + kLanes <= count; i += kLanes)
    O::save(output + i,
            evaluate<K, O>(O::load(background + i), O::load(source + i)));
  return i;
}
#endif

#if defined(PHOTOSPIDER_HAS_BLEND_SME)
inline bool sme_available() {
  static const bool available = [] {
    int enabled = 0;
    std::size_t size = sizeof(enabled);
    if (sysctlbyname("hw.optional.arm.FEAT_SME", &enabled, &size, nullptr, 0) ||
        size != sizeof(enabled) || !enabled)
      return false;
    return sme_vector_bytes() == 64;
  }();
  return available;
}
#endif

/** Contiguous, disjoint output; Normal never dereferences background. */
template <Kind K, class T>
inline void span(const T* background, const T* source, T* output,
                 std::size_t count, std::size_t footprint_bytes = 0) {
  const auto bytes = count * sizeof(T);
  if (!footprint_bytes)
    footprint_bytes = bytes;
#if defined(PHOTOSPIDER_HAS_BLEND_SME)
  if constexpr (K == Kind::Normal) {
    if (bytes >= 4096 && footprint_bytes <= 8 * 1024 * 1024 &&
        sme_available()) {
      sme_copy(source, output, bytes);
      return;
    }
  }
  if constexpr (std::is_same_v<T, float> &&
                ((K >= Kind::Add && K <= Kind::Lighten && K != Kind::Divide) ||
                 K == Kind::Difference)) {
    if (bytes >= 4096 && footprint_bytes >= 128 * 1024 &&
        footprint_bytes <= 4 * 1024 * 1024 && sme_available()) {
      sme_binary(K, background, source, output, count);
      return;
    }
  }
#if defined(PHOTOSPIDER_HAS_BLEND_SME_F64)
  if constexpr (std::is_same_v<T, double> && K == Kind::Difference) {
    static const bool fp64 = [] {
      int enabled = 0;
      std::size_t size = sizeof(enabled);
      return sysctlbyname("hw.optional.arm.FEAT_SME_F64F64", &enabled, &size,
                          nullptr, 0) == 0 &&
             size == sizeof(enabled) && enabled;
    }();
    if (bytes >= 4096 && footprint_bytes >= 128 * 1024 &&
        footprint_bytes <= 8 * 1024 * 1024 && fp64 && sme_available()) {
      sme_difference(background, source, output, count);
      return;
    }
  }
#endif
#endif
  if constexpr (K == Kind::Normal) {
    std::memcpy(output, source, count * sizeof(T));
  } else {
    std::size_t i = 0;
#if defined(__aarch64__)
    i = footprint_bytes >= 256 * 1024
            ? packets<K, T, true>(background, source, output, count)
            : packets<K, T, false>(background, source, output, count);
#else
    (void)footprint_bytes;
#endif
    for (; i < count; ++i)
      output[i] = evaluate<K, Scalar<T>>(background[i], source[i]);
  }
}
}  // namespace ps::plugin_internal::blend
