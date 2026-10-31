// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "01-numeric/fast_dispatch.hpp"
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace ps::plugin_internal::fast_sampling {
struct Polynomial final {
  std::array<double, 9> coefficients{};
  unsigned degree = 0;
  double evaluate(double x) const {
    double value = coefficients[degree];
    for (unsigned j = degree; j-- > 0;) {
      value = std::fma(value, x, coefficients[j]);
    }
    return value;
  }
};
// Nonnegative sum/product polynomials on [0,1] have no cancellation. Horner
// reduces both temporary traffic and rounding stages. Other ASTs retain their
// native instruction order; this is not an unrestricted algebraic rewrite.
inline bool polynomial(const numeric_ops::ExpressionProgram& program,
                       const double* coefficients, Polynomial* output,
                       double* workspace) {
  using K = numeric_ops::ExpressionKind;
  std::array<unsigned, 256> degrees{};
  constexpr unsigned width = 9;
  for (unsigned node = 0; node < program.size; ++node) {
    const auto& op = program.nodes[node];
    auto* p = workspace + width * node;
    std::fill_n(p, width, 0.0);
    const auto* a = workspace + width * op.left;
    const auto* b = workspace + width * op.right;
    if (op.kind == K::Literal || op.kind == K::Coefficient) {
      p[0] = op.kind == K::Literal ? numeric_ops::numeric_double(op.bits)
                                   : coefficients[op.left];
    } else if (op.kind == K::X) {
      p[1] = 1;
      degrees[node] = 1;
    } else if (op.kind == K::Positive) {
      std::copy_n(a, width, p);
      degrees[node] = degrees[op.left];
    } else if (op.kind == K::Add) {
      degrees[node] = std::max(degrees[op.left], degrees[op.right]);
      for (unsigned j = 0; j <= degrees[node]; ++j) {
        p[j] = a[j] + b[j];
      }
    } else if (op.kind == K::Multiply) {
      const auto da = degrees[op.left], db = degrees[op.right];
      if (da + db >= width) {
        return false;
      }
      degrees[node] = da + db;
      for (unsigned i = 0; i <= da; ++i) {
        for (unsigned j = 0; j <= db; ++j) {
          p[i + j] = std::fma(a[i], b[j], p[i + j]);
        }
      }
    } else {
      return false;
    }
    for (unsigned j = 0; j <= degrees[node]; ++j) {
      if (!(p[j] >= 0) || !std::isfinite(p[j])) {
        return false;
      }
    }
  }
  output->degree = degrees[program.size - 1];
  std::copy_n(workspace + width * (program.size - 1), width,
              output->coefficients.data());
  return true;
}
template <unsigned Degree, class T>
inline void polynomial_span(const Polynomial& p, double start, double end,
                            std::uint64_t total, std::uint64_t first, T* output,
                            std::size_t count) {
  const double inv = total > 1 ? 1.0 / (total - 1) : 0;
  const double difference = end - start;
  std::size_t i = 0;
#if defined(__aarch64__)
  std::array<float64x2_t, 9> coefficients;
  for (unsigned j = 0; j <= Degree; ++j) {
    coefficients[j] = vdupq_n_f64(p.coefficients[j]);
  }
  const auto scale = vdupq_n_f64(inv), delta = vdupq_n_f64(difference),
             begin = vdupq_n_f64(start);
  for (; i + 2 <= count; i += 2) {
    const double indexes[]{static_cast<double>(first + i),
                           static_cast<double>(first + i + 1)};
    const auto x =
        vfmaq_f64(begin, vmulq_f64(vld1q_f64(indexes), scale), delta);
    auto value = coefficients[Degree];
    for (unsigned j = Degree; j-- > 0;) {
      value = vfmaq_f64(coefficients[j], value, x);
    }
    if constexpr (sizeof(T) == 4) {
      vst1_f32(output + i, vcvt_f32_f64(value));
    } else {
      vst1q_f64(output + i, value);
    }
  }
#elif defined(__x86_64__)
  std::array<__m128d, 9> coefficients;
  for (unsigned j = 0; j <= Degree; ++j) {
    coefficients[j] = _mm_set1_pd(p.coefficients[j]);
  }
  const auto scale = _mm_set1_pd(inv), delta = _mm_set1_pd(difference),
             begin = _mm_set1_pd(start);
  for (; i + 2 <= count; i += 2) {
    const auto index = _mm_set_pd(static_cast<double>(first + i + 1),
                                  static_cast<double>(first + i));
    const auto x =
        _mm_add_pd(begin, _mm_mul_pd(_mm_mul_pd(index, scale), delta));
    auto value = coefficients[Degree];
    for (unsigned j = Degree; j-- > 0;) {
      value = _mm_add_pd(coefficients[j], _mm_mul_pd(value, x));
    }
    if constexpr (sizeof(T) == 4) {
      const auto small = _mm_cvtpd_ps(value);
      _mm_storel_pi(reinterpret_cast<__m64*>(output + i), small);
    } else {
      _mm_storeu_pd(output + i, value);
    }
  }
#endif
  for (; i < count; ++i) {
    output[i] =
        static_cast<T>(p.evaluate(coordinate(start, end, first + i, total)));
  }
  if (count && first == 0) {
    output[0] = static_cast<T>(p.evaluate(start));
  }
  if (count && first + count == total) {
    output[count - 1] = static_cast<T>(p.evaluate(end));
  }
}
template <class T>
inline void sample_polynomial(const Polynomial& p, double start, double end,
                              std::uint64_t total, std::uint64_t first,
                              T* output, std::size_t count) {
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::polynomial(sizeof(T) == 4, p, start, end, total, first,
                               output, count);
    return;
  }
#endif
  switch (p.degree) {
#define PS_NATIVE_POLYNOMIAL(D)                                     \
  case D:                                                           \
    polynomial_span<D>(p, start, end, total, first, output, count); \
    break
    PS_NATIVE_POLYNOMIAL(0);
    PS_NATIVE_POLYNOMIAL(1);
    PS_NATIVE_POLYNOMIAL(2);
    PS_NATIVE_POLYNOMIAL(3);
    PS_NATIVE_POLYNOMIAL(4);
    PS_NATIVE_POLYNOMIAL(5);
    PS_NATIVE_POLYNOMIAL(6);
    PS_NATIVE_POLYNOMIAL(7);
    PS_NATIVE_POLYNOMIAL(8);
#undef PS_NATIVE_POLYNOMIAL
  }
}
template <class T>
inline void coordinates(double start, double end, std::uint64_t total,
                        std::uint64_t first, T* output, std::size_t count) {
#if defined(__x86_64__)
  if (native_compute::available()) {
    native_compute::coordinates(sizeof(T) == 4, start, end, total, first,
                                output, count);
    return;
  }
#endif
  if (std::isfinite(end - start)) {
    Polynomial identity;
    identity.degree = 1;
    identity.coefficients[1] = 1;
    polynomial_span<1>(identity, start, end, total, first, output, count);
    if (count && first == 0) {
      output[0] = static_cast<T>(start);
    }
    if (count && first + count == total) {
      output[count - 1] = static_cast<T>(end);
    }
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      output[i] = static_cast<T>(coordinate(start, end, first + i, total));
    }
  }
}
}  // namespace ps::plugin_internal::fast_sampling
