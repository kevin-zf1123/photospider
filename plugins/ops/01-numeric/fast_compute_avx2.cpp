// Copyright 2026 Photospider contributors
#include <immintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "01-numeric/fast_curve.hpp"
#include "01-numeric/fast_sampling.hpp"
#include "01-numeric/vector_lut.hpp"
#include "02-format-color/fast_color.hpp"
#include "02-format-color/fast_color_avx2.hpp"
#include "07-grade/curves_math.hpp"
#include "07-grade/gradient_map_math.hpp"

namespace ps::plugin_internal::native_compute {
namespace {
using V = __m256d;
void cube_roots_refined(const double*, double*, std::size_t, unsigned);
inline V set(double x) {
  return _mm256_set1_pd(x);
}
inline V choose(V mask, V yes, V no) {
  return _mm256_blendv_pd(no, yes, mask);
}
inline V eq(V a, V b) {
  return _mm256_cmp_pd(a, b, _CMP_EQ_OQ);
}
inline V ge(V a, V b) {
  return _mm256_cmp_pd(a, b, _CMP_GE_OQ);
}
inline V le(V a, V b) {
  return _mm256_cmp_pd(a, b, _CMP_LE_OQ);
}
template <class T>
V load4(const T* p) {
  if constexpr (sizeof(T) == 4) {
    return _mm256_cvtps_pd(_mm_loadu_ps(p));
  } else {
    return _mm256_loadu_pd(p);
  }
}
template <class T>
void store4(T* p, V v) {
  if constexpr (sizeof(T) == 4) {
    _mm_storeu_ps(p, _mm256_cvtpd_ps(v));
  } else {
    _mm256_storeu_pd(p, v);
  }
}
template <class T, unsigned C>
void store_columns(T* output, const std::array<V, C>& v) {
  if constexpr (C == 1) {
    store4(output, v[0]);
  } else if constexpr (sizeof(T) == 4) {
    const std::array<__m128, 3> packed{
        _mm256_cvtpd_ps(v[0]), _mm256_cvtpd_ps(v[1]), _mm256_cvtpd_ps(v[2])};
    fast_color::Packet<float>::store3(output, packed);
  } else {
    fast_color::avx2::Packet<double>::store3(output, v);
  }
}
template <class T>
void matrix_t(const fast_color::Matrix&, const void*, void*, std::size_t);
template <unsigned Kind, class T>
void model_t(const fast_color::Model& m, const T* input, T* output,
             std::size_t n, std::size_t ic, std::size_t oc,
             const std::array<std::uint64_t, 3>& selected) {
  if constexpr (Kind == 16) {
    const auto c = selected[m.gray == 0 ? 1 : 0];
    for (std::size_t i = 0; i < n; ++i) {
      std::memcpy(output + i * oc, input + i * ic + c, sizeof(T));
    }
    return;
  }
  if constexpr (Kind == 12 || Kind == 13) {
    if (ic == 3 && oc == 3 &&
        selected == std::array<std::uint64_t, 3>{0, 1, 2}) {
      matrix_t<T>(m.first, input, output, n);
      return;
    }
  }
  if constexpr (Kind == 0 || Kind == 4) {
    if (ic == 3 && oc == 3 &&
        selected == std::array<std::uint64_t, 3>{0, 1, 2}) {
      constexpr std::size_t block = 128;
      alignas(64) double args[3][block], roots[3][block];
      for (std::size_t first = 0; first < n; first += block) {
        const auto length = std::min(block, n - first);
        for (std::size_t j = 0; j < length; ++j) {
          const auto i = first + j;
          fast_color::Triple value{input[3 * i], input[3 * i + 1],
                                   input[3 * i + 2]};
          if constexpr (Kind == 0) {
            value = {value[0] / m.white[0], value[1], value[2] / m.white[2]};
          } else {
            value = fast_color::multiply(m.first, value);
          }
          for (unsigned c = 0; c < 3; ++c) {
            args[c][j] = value[c];
          }
        }
        for (unsigned c = 0; c < 3; ++c) {
          cube_roots(args[c], roots[c], length);
        }
        for (std::size_t j = 0; j < length; ++j) {
          fast_color::Triple value{roots[0][j], roots[1][j], roots[2][j]};
          if constexpr (Kind == 0) {
            for (unsigned c = 0; c < 3; ++c) {
              if (args[c][j] <= fast_color::kEpsilon) {
                value[c] =
                    args[c][j] / (3 * fast_color::kDelta * fast_color::kDelta) +
                    4.0 / 29;
              }
            }
            value = {1.16 * value[1] - .16, 500 * (value[0] - value[1]),
                     200 * (value[1] - value[2])};
          } else {
            value = fast_color::multiply(m.second, value);
          }
          for (unsigned c = 0; c < 3; ++c) {
            output[3 * (first + j) + c] = static_cast<T>(value[c]);
          }
        }
      }
      return;
    }
  }
  if (fast_color::transcendental_span<Kind>(m, input, output, n, ic, oc,
                                            selected) ||
      fast_color::packet_span_impl<Kind, T, fast_color::avx2::Packet<T>>(
          m, input, output, n, ic, oc, selected)) {
    return;
  }
  if constexpr (Kind == 19) {
    const auto constants = m.template levels<T>();
    for (std::size_t i = 0; i < n; ++i) {
      std::memcpy(output + i * oc,
                  &constants[input[i * ic + selected[0]] == 0 ? 0 : 1],
                  sizeof(T));
    }
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    fast_color::Triple x{};
    constexpr unsigned inputs = Kind >= 17 ? 1 : 3;
    for (unsigned c = 0; c < inputs; ++c) {
      x[c] = input[i * ic + selected[c]];
    }
    const auto y = m.apply<Kind>(x);
    constexpr unsigned outputs = Kind == 16 || Kind >= 18 ? 1 : 3;
    for (unsigned c = 0; c < outputs; ++c) {
      output[i * oc + c] = static_cast<T>(y[c]);
    }
  }
}
template <class T>
void model_dispatch(unsigned kind, const fast_color::Model& m,
                    const void* input, void* output, std::size_t n,
                    std::size_t ic, std::size_t oc,
                    const std::array<std::uint64_t, 3>& selected) {
  const auto* x = static_cast<const T*>(input);
  auto* y = static_cast<T*>(output);
  switch (kind) {
#define PS_MODEL(K)                           \
  case K:                                     \
    model_t<K>(m, x, y, n, ic, oc, selected); \
    break
    PS_MODEL(0);
    PS_MODEL(1);
    PS_MODEL(2);
    PS_MODEL(3);
    PS_MODEL(4);
    PS_MODEL(5);
    PS_MODEL(6);
    PS_MODEL(7);
    PS_MODEL(8);
    PS_MODEL(9);
    PS_MODEL(10);
    PS_MODEL(11);
    PS_MODEL(12);
    PS_MODEL(13);
    PS_MODEL(14);
    PS_MODEL(15);
    PS_MODEL(16);
    PS_MODEL(17);
    PS_MODEL(18);
    PS_MODEL(19);
#undef PS_MODEL
  }
}
template <class T>
void matrix_t(const fast_color::Matrix& m, const void* in, void* out,
              std::size_t n) {
  const auto* x = static_cast<const T*>(in);
  auto* y = static_cast<T*>(out);
  std::size_t first = 0;
  if constexpr (sizeof(T) == 4) {
    const auto ri = _mm256_setr_epi32(0, 0, 0, 3, 3, 3, 0, 0),
               gi = _mm256_setr_epi32(1, 1, 1, 4, 4, 4, 1, 1),
               bi = _mm256_setr_epi32(2, 2, 2, 5, 5, 5, 2, 2);
    const auto a = _mm256_setr_ps(m[0][0], m[1][0], m[2][0], m[0][0], m[1][0],
                                  m[2][0], m[0][0], m[1][0]);
    const auto b = _mm256_setr_ps(m[0][1], m[1][1], m[2][1], m[0][1], m[1][1],
                                  m[2][1], m[0][1], m[1][1]);
    const auto c = _mm256_setr_ps(m[0][2], m[1][2], m[2][2], m[0][2], m[1][2],
                                  m[2][2], m[0][2], m[1][2]);
    const auto evaluate = [&](std::size_t i) {
      const auto packed = _mm256_loadu_ps(x + 3 * i);
      const auto r = _mm256_permutevar8x32_ps(packed, ri);
      const auto g = _mm256_permutevar8x32_ps(packed, gi);
      const auto bvalue = _mm256_permutevar8x32_ps(packed, bi);
      return _mm256_fmadd_ps(c, bvalue,
                             _mm256_fmadd_ps(b, g, _mm256_mul_ps(a, r)));
    };
    const auto store = [&](std::size_t i, __m256 value) {
      // Only the two completed pixels are written; extra lanes are read-only.
      _mm_storeu_ps(y + 3 * i, _mm256_castps256_ps128(value));
      _mm_storel_epi64(reinterpret_cast<__m128i*>(y + 3 * i + 4),
                       _mm_castps_si128(_mm256_extractf128_ps(value, 1)));
    };
    for (; first + 10 <= n; first += 8) {
      // Finish a group of source reads before writing to limit address-alias
      // stalls when disjoint buffers have nearby page offsets.
      std::array<__m256, 4> values;
#pragma clang loop unroll(full)
      for (unsigned j = 0; j < 4; ++j) {
        values[j] = evaluate(first + 2 * j);
      }
#pragma clang loop unroll(full)
      for (unsigned j = 0; j < 4; ++j) {
        store(first + 2 * j, values[j]);
      }
    }
    for (; first + 4 <= n; first += 2) {
      store(first, evaluate(first));
    }
  } else {
    first = fast_color::packet_matrix_impl<T, fast_color::avx2::Packet<T>>(
        m, x, y, n);
  }
  for (std::size_t i = first; i < n; ++i) {
    const auto v = fast_color::multiply(
        m, fast_color::Triple{x[3 * i], x[3 * i + 1], x[3 * i + 2]});
    for (unsigned c = 0; c < 3; ++c) {
      y[3 * i + c] = static_cast<T>(v[c]);
    }
  }
}
template <unsigned C, bool Cubic, class Input, class Output>
void small_curves(const fast_curve::Curve* curves, const Input* input,
                  Output* output, std::size_t count) {
  const auto& axis = curves[0];
  std::array<double, 8> starts{};
  std::fill(starts.begin(), starts.end(), INFINITY);
  std::copy_n(axis.x, axis.size - 1, starts.begin());
  const auto first = set(axis.x[0]), last = set(axis.x[axis.size - 1]);
  std::size_t j = 0;
  for (; j + 4 <= count; j += 4) {
    const auto q = load4(input + j);
    auto index = _mm256_setzero_si256();
    for (unsigned step : {4U, 2U, 1U}) {
      const auto mid = _mm256_add_epi64(index, _mm256_set1_epi64x(step));
      const auto pivot = _mm256_i64gather_pd(starts.data(), mid, 8);
      index = _mm256_blendv_epi8(index, mid, _mm256_castpd_si256(ge(q, pivot)));
    }
    const auto x = _mm256_i64gather_pd(axis.x, index, 8);
    const auto delta = _mm256_sub_pd(q, x);
    const auto t =
        Cubic ? _mm256_mul_pd(delta, _mm256_i64gather_pd(axis.slope, index, 8))
              : delta;
    std::array<V, C> values;
    for (unsigned c = 0; c < C; ++c) {
      const auto& curve = curves[c];
      const auto y = _mm256_i64gather_pd(curve.y, index, 8);
      V value;
      if (curve.identity) {
        value = choose(eq(q, set(0)), set(0), q);
      } else if (curve.constant) {
        value = set(curve.identity_or_constant(0));
        if (curve.edge == fast_curve::Edge::Linear && curve.y[0] == 0) {
          value = choose(_mm256_cmp_pd(q, first, _CMP_LT_OQ),
                         set(curve.first_constant), value);
          value = choose(_mm256_cmp_pd(q, last, _CMP_GT_OQ),
                         set(curve.last_constant), value);
        }
      } else {
        const auto a = _mm256_i64gather_pd(curve.a, index, 8);
        if constexpr (Cubic) {
          value = _mm256_fmadd_pd(
              t,
              _mm256_fmadd_pd(
                  t,
                  _mm256_fmadd_pd(t, _mm256_i64gather_pd(curve.c, index, 8),
                                  _mm256_i64gather_pd(curve.b, index, 8)),
                  a),
              y);
        } else {
          value = _mm256_fmadd_pd(t, a, y);
        }
      }
      value = choose(eq(q, x), y, value);
      if (curve.edge == fast_curve::Edge::Linear) {
        if (!curve.identity && !curve.constant) {
          value =
              choose(_mm256_cmp_pd(q, first, _CMP_LT_OQ),
                     _mm256_fmadd_pd(_mm256_sub_pd(q, first),
                                     set(curve.first_slope), set(curve.y[0])),
                     value);
          value = choose(
              _mm256_cmp_pd(q, last, _CMP_GT_OQ),
              _mm256_fmadd_pd(_mm256_sub_pd(q, last), set(curve.last_slope),
                              set(curve.y[axis.size - 1])),
              value);
        }
        value = choose(eq(q, first), set(curve.y[0]), value);
        value = choose(eq(q, last), set(curve.y[axis.size - 1]), value);
      } else {
        value = choose(le(q, first), set(curve.y[0]), value);
        value = choose(ge(q, last), set(curve.y[axis.size - 1]), value);
      }
      values[c] = value;
    }
    store_columns<Output, C>(output + j * C, values);
  }
  for (; j < count; ++j) {
    for (unsigned c = 0; c < C; ++c) {
      output[j * C + c] = static_cast<Output>(curves[c].evaluate(input[j]));
    }
  }
}
template <unsigned C, bool Cubic, class Input, class Output>
void ordered_curves(const fast_curve::Curve* curves, const Input* input,
                    Output* output, std::size_t count, int direction) {
  const auto& axis = curves[0];
  std::size_t begin = 0;
  while (begin < count) {
    const double query = input[begin];
    if (!(query > axis.x[0] && query < axis.x[axis.size - 1])) {
      for (unsigned c = 0; c < C; ++c) {
        output[begin * C + c] = static_cast<Output>(curves[c].evaluate(query));
      }
      ++begin;
      continue;
    }
    const auto segment = axis.segment(query);
    auto low = begin + 1, high = count;
    while (low < high) {
      const auto mid = low + (high - low) / 2;
      const bool inside = direction > 0 ? input[mid] < axis.x[segment + 1]
                                        : input[mid] >= axis.x[segment];
      if (inside) {
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    const auto end = low;
    const auto zero = set(0);
    const auto origin = set(axis.x[segment]),
               inverse = set(axis.slope[segment]);
    std::array<V, C> a, b, c, y;
    for (unsigned k = 0; k < C; ++k) {
      a[k] = set(curves[k].a[segment]);
      b[k] = set(Cubic ? curves[k].b[segment] : 0);
      c[k] = set(Cubic ? curves[k].c[segment] : 0);
      y[k] = set(curves[k].y[segment]);
    }
    const auto evaluate = [&](std::size_t i) {
      const auto q = load4(input + i);
      const auto delta = _mm256_sub_pd(q, origin);
      const auto t = Cubic ? _mm256_mul_pd(delta, inverse) : delta;
      std::array<V, C> value;
      for (unsigned k = 0; k < C; ++k) {
        if constexpr (Cubic) {
          value[k] = _mm256_fmadd_pd(
              t, _mm256_fmadd_pd(t, _mm256_fmadd_pd(t, c[k], b[k]), a[k]),
              y[k]);
        } else {
          value[k] = _mm256_fmadd_pd(t, a[k], y[k]);
        }
        if (curves[k].identity) {
          value[k] = choose(eq(q, zero), zero, q);
        } else if (curves[k].constant) {
          value[k] = set(curves[k].identity_or_constant(0));
        }
        value[k] = choose(eq(q, origin), y[k], value[k]);
      }
      return value;
    };
    if constexpr (C == 1) {
      for (; begin + 16 <= end; begin += 16) {
        std::array<std::array<V, C>, 4> values;
#pragma clang loop unroll(full)
        for (unsigned j = 0; j < 4; ++j) {
          values[j] = evaluate(begin + 4 * j);
        }
#pragma clang loop unroll(full)
        for (unsigned j = 0; j < 4; ++j) {
          store_columns<Output, C>(output + begin + 4 * j, values[j]);
        }
      }
    }
    for (; begin + 4 <= end; begin += 4) {
      store_columns<Output, C>(output + begin * C, evaluate(begin));
    }
    for (; begin < end; ++begin) {
      for (unsigned k = 0; k < C; ++k) {
        output[begin * C + k] =
            static_cast<Output>(curves[k].evaluate(input[begin]));
      }
    }
  }
}
template <class Input, class Output>
void curve_t(const fast_curve::Curve* curves, unsigned columns, const void* in,
             void* out, std::size_t count) {
  const auto* input = static_cast<const Input*>(in);
  auto* output = static_cast<Output*>(out);
  int direction = 0;
  if (count > 1) {
    direction = input[0] <= input[count - 1] ? 1 : -1;
    for (std::size_t j = 1; j < count; ++j) {
      if (!(direction > 0 ? input[j] >= input[j - 1]
                          : input[j] <= input[j - 1])) {
        direction = 0;
        break;
      }
    }
  }
#define PS_CURVE_RUN(C, B)                                           \
  do {                                                               \
    if (direction) {                                                 \
      ordered_curves<C, B>(curves, input, output, count, direction); \
    } else {                                                         \
      small_curves<C, B>(curves, input, output, count);              \
    }                                                                \
  } while (false)
  if (count >= 16 && (columns == 1 || columns == 3) &&
      (direction || curves[0].size <= 8)) {
    if (columns == 1) {
      if (curves[0].cubic) {
        PS_CURVE_RUN(1, true);
      } else {
        PS_CURVE_RUN(1, false);
      }
    } else {
      if (curves[0].cubic) {
        PS_CURVE_RUN(3, true);
      } else {
        PS_CURVE_RUN(3, false);
      }
    }
    return;
  }
#undef PS_CURVE_RUN
  fast_curve::shared(
      curves, columns, [input](auto j) { return input[j]; }, output, count,
      direction);
}
template <class Input, class Output>
void lut_t(const void* in, void* out, std::size_t count, const double* values,
           std::size_t size, double start, double end, fast_curve::Edge edge,
           std::ptrdiff_t input_step, std::ptrdiff_t output_step) {
  const auto* input = static_cast<const Input*>(in);
  auto* output = static_cast<Output*>(out);
  if (size == 1) {
    for (std::size_t j = 0; j < count; ++j) {
      output[j * output_step] = static_cast<Output>(values[0]);
    }
    return;
  }
  const double factor = (size - 1) / (end - start);
  if (!std::isfinite(factor) || factor == 0) {
    for (std::size_t j = 0; j < count; ++j)
      output[j * output_step] = static_cast<Output>(fast_curve::lut(
          input[j * input_step], values, size, start, end, edge));
    return;
  }
  const auto scale = set(factor), low = set(start), high = set(end);
  const auto zero = set(0), one = set(1), limit = set(size - 1),
             cap = set(size - 2);
  std::size_t j = 0;
  for (; j + 4 <= count; j += 4) {
    V q;
    if (input_step == 1) {
      q = load4(input + j);
    } else {
      q = _mm256_set_pd(input[(j + 3) * input_step],
                        input[(j + 2) * input_step],
                        input[(j + 1) * input_step], input[j * input_step]);
    }
    auto u = _mm256_mul_pd(_mm256_sub_pd(q, low), scale);
    if (edge == fast_curve::Edge::Linear &&
        _mm256_movemask_pd(eq(_mm256_andnot_pd(set(-0.0), u), set(HUGE_VAL)))) {
      for (unsigned k = 0; k < 4; ++k)
        output[(j + k) * output_step] = static_cast<Output>(fast_curve::lut(
            input[(j + k) * input_step], values, size, start, end, edge));
      continue;
    }
    if (edge != fast_curve::Edge::Linear) {
      u = _mm256_min_pd(limit, _mm256_max_pd(zero, u));
    }
    const auto position =
        _mm256_min_pd(cap, _mm256_max_pd(zero, _mm256_floor_pd(u)));
    const auto index32 = _mm256_cvttpd_epi32(position);
    const auto index = _mm256_cvtepi32_epi64(index32);
    const auto a = _mm256_i64gather_pd(values, index, 8);
    const auto b = _mm256_i64gather_pd(
        values, _mm256_add_epi64(index, _mm256_set1_epi64x(1)), 8);
    const auto t = _mm256_sub_pd(u, position);
    auto value = _mm256_fmadd_pd(t, _mm256_sub_pd(b, a), a);
    value = choose(eq(t, zero), a, value);
    value = choose(eq(t, one), b, value);
    value = choose(eq(q, low), set(values[0]), value);
    value = choose(eq(q, high), set(values[size - 1]), value);
    if (output_step == 1) {
      store4(output + j, value);
    } else {
      std::array<double, 4> packed;
      _mm256_storeu_pd(packed.data(), value);
      for (unsigned k = 0; k < 4; ++k) {
        output[(j + k) * output_step] = static_cast<Output>(packed[k]);
      }
    }
  }
  for (; j < count; ++j) {
    output[j * output_step] = static_cast<Output>(
        fast_curve::lut(input[j * input_step], values, size, start, end, edge));
  }
}
template <unsigned Degree, class T>
void polynomial_t(const fast_sampling::Polynomial& p, double start, double end,
                  std::uint64_t total, std::uint64_t first, T* output,
                  std::size_t count) {
  const auto inv = set(total > 1 ? 1.0 / (total - 1) : 0),
             delta = set(end - start), begin = set(start);
  std::array<V, 9> coefficients;
  for (unsigned j = 0; j <= Degree; ++j) {
    coefficients[j] = set(p.coefficients[j]);
  }
  std::size_t j = 0;
  // Public sampling extents are at most 2^20; these integer indices and
  // increments are exactly representable. The floating grid is not summed.
  auto index = _mm256_set_pd(first + 3, first + 2, first + 1, first);
  const auto increment = set(4);
  for (; j + 4 <= count; j += 4) {
    const auto q = _mm256_fmadd_pd(_mm256_mul_pd(index, inv), delta, begin);
    index = _mm256_add_pd(index, increment);
    auto value = coefficients[Degree];
    for (unsigned c = Degree; c-- > 0;) {
      value = _mm256_fmadd_pd(value, q, coefficients[c]);
    }
    store4(output + j, value);
  }
  for (; j < count; ++j) {
    output[j] = static_cast<T>(
        p.evaluate(fast_sampling::coordinate(start, end, first + j, total)));
  }
  if (count && first == 0) {
    output[0] = static_cast<T>(p.evaluate(start));
  }
  if (count && first + count == total) {
    output[count - 1] = static_cast<T>(p.evaluate(end));
  }
}
template <class T>
void polynomial_dispatch(const fast_sampling::Polynomial& p, double start,
                         double end, std::uint64_t total, std::uint64_t first,
                         void* out, std::size_t count) {
  auto* output = static_cast<T*>(out);
  switch (p.degree) {
#define PS_POLY(D)                                               \
  case D:                                                        \
    polynomial_t<D>(p, start, end, total, first, output, count); \
    break
    PS_POLY(0);
    PS_POLY(1);
    PS_POLY(2);
    PS_POLY(3);
    PS_POLY(4);
    PS_POLY(5);
    PS_POLY(6);
    PS_POLY(7);
    PS_POLY(8);
#undef PS_POLY
  }
}
template <class T>
void brightness_t(unsigned channels, const grade::curves::Brightness& b,
                  const fast_curve::Curve& curve, const void* in, void* out,
                  std::size_t count) {
  const auto* input = static_cast<const T*>(in);
  auto* output = static_cast<T*>(out);
  if (b.route == 3) {
    constexpr unsigned batch = 128;
    alignas(64) double lms[3][batch], roots[3][batch];
    for (std::size_t first = 0; first < count; first += batch) {
      const auto n = std::min<std::size_t>(batch, count - first);
      for (unsigned j = 0; j < n; ++j) {
        const auto i = first + j;
        const auto value = fast_color::multiply(
            b.rgb_to_lms,
            fast_color::Triple{input[i * channels], input[i * channels + 1],
                               input[i * channels + 2]});
        for (unsigned c = 0; c < 3; ++c) {
          lms[c][j] = value[c];
        }
      }
      for (unsigned c = 0; c < 3; ++c) {
        cube_roots(lms[c], roots[c], n);
      }
      for (unsigned j = 0; j < n; ++j) {
        const auto i = first + j;
        const fast_color::Triple rgb{input[i * channels],
                                     input[i * channels + 1],
                                     input[i * channels + 2]};
        bool changed;
        const auto value = b.from_roots(
            rgb, {roots[0][j], roots[1][j], roots[2][j]}, curve, &changed);
        if (!changed) {
          std::memcpy(output + i * channels, input + i * channels,
                      channels * sizeof(T));
        } else {
          for (unsigned c = 0; c < 3; ++c) {
            output[i * channels + c] = static_cast<T>(value[c]);
          }
          if (channels == 4) {
            std::memcpy(output + i * channels + 3, input + i * channels + 3,
                        sizeof(T));
          }
        }
      }
    }
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const fast_color::Triple rgb{input[i * channels], input[i * channels + 1],
                                 input[i * channels + 2]};
    bool changed;
    const auto value = b.apply(rgb, curve, &changed);
    if (!changed) {
      std::memcpy(output + i * channels, input + i * channels,
                  channels * sizeof(T));
    } else {
      for (unsigned c = 0; c < 3; ++c) {
        output[i * channels + c] = static_cast<T>(value[c]);
      }
      if (channels == 4) {
        std::memcpy(output + i * channels + 3, input + i * channels + 3,
                    sizeof(T));
      }
    }
  }
}
template <class T>
void components_t(unsigned channels, unsigned selected,
                  const fast_curve::Curve* curves, const void* in, void* out,
                  std::size_t count) {
  const auto* input = static_cast<const T*>(in);
  auto* output = static_cast<T*>(out);
  for (std::size_t i = 0; i < count; ++i) {
    for (unsigned c = 0; c < channels; ++c) {
      if (c < selected) {
        output[i * channels + c] =
            static_cast<T>(curves[c].evaluate(input[i * channels + c]));
      } else {
        std::memcpy(output + i * channels + c, input + i * channels + c,
                    sizeof(T));
      }
    }
  }
}
template <class T>
void store_four_columns(T* target, const std::array<V, 4>& values) {
  if constexpr (sizeof(T) == 4) {
    auto a = _mm256_cvtpd_ps(values[0]), b = _mm256_cvtpd_ps(values[1]);
    auto c = _mm256_cvtpd_ps(values[2]), d = _mm256_cvtpd_ps(values[3]);
    _MM_TRANSPOSE4_PS(a, b, c, d);
    _mm_storeu_ps(target, a);
    _mm_storeu_ps(target + 4, b);
    _mm_storeu_ps(target + 8, c);
    _mm_storeu_ps(target + 12, d);
  } else {
    const auto rg0 = _mm256_unpacklo_pd(values[0], values[1]);
    const auto rg1 = _mm256_unpackhi_pd(values[0], values[1]);
    const auto ba0 = _mm256_unpacklo_pd(values[2], values[3]);
    const auto ba1 = _mm256_unpackhi_pd(values[2], values[3]);
    _mm256_storeu_pd(target, _mm256_permute2f128_pd(rg0, ba0, 0x20));
    _mm256_storeu_pd(target + 4, _mm256_permute2f128_pd(rg1, ba1, 0x20));
    _mm256_storeu_pd(target + 8, _mm256_permute2f128_pd(rg0, ba0, 0x31));
    _mm256_storeu_pd(target + 12, _mm256_permute2f128_pd(rg1, ba1, 0x31));
  }
}
// Large constant results have no input locality to retain. Streaming stores
// avoid read-for-ownership traffic; the fence retires stores before return.
template <unsigned Channels, class Output>
void constant_stream(const double* table, Output* output, std::size_t count) {
  constexpr unsigned lanes = 32 / sizeof(Output);
  Output row[Channels];
  for (unsigned c = 0; c < Channels; ++c)
    row[c] = static_cast<Output>(table[c]);
  const auto elements = count * Channels;
  std::size_t j = 0;
  for (; j < elements && reinterpret_cast<std::uintptr_t>(output + j) % 32; ++j)
    output[j] = row[j % Channels];
  alignas(32) Output pattern[Channels * lanes];
  for (unsigned c = 0; c < Channels * lanes; ++c)
    pattern[c] = row[(j + c) % Channels];
  if constexpr (sizeof(Output) == 4) {
    std::array<__m256, Channels> values;
    for (unsigned c = 0; c < Channels; ++c)
      values[c] = _mm256_load_ps(pattern + c * lanes);
    for (; j + Channels * lanes <= elements; j += Channels * lanes)
      for (unsigned c = 0; c < Channels; ++c)
        _mm256_stream_ps(output + j + c * lanes, values[c]);
  } else {
    std::array<V, Channels> values;
    for (unsigned c = 0; c < Channels; ++c)
      values[c] = _mm256_load_pd(pattern + c * lanes);
    for (; j + Channels * lanes <= elements; j += Channels * lanes)
      for (unsigned c = 0; c < Channels; ++c)
        _mm256_stream_pd(output + j + c * lanes, values[c]);
  }
  _mm_sfence();
  for (; j < elements; ++j)
    output[j] = row[j % Channels];
}
template <unsigned Channels, bool SharedRows, class Input, class Output>
void vector_lut_rows(const fast_curve::VectorLut& lut, const Input* input,
                     Output* output, std::size_t count, std::ptrdiff_t stride) {
  const auto zero = set(0), one = set(1), first = set(lut.start),
             last = set(lut.end);
  const auto factor = set(lut.scale), cap = set(lut.length - 2),
             limit = set(lut.length - 1);
  std::size_t j = 0;
  for (; j + 4 <= count; j += 4) {
    const auto index = static_cast<std::ptrdiff_t>(j) * stride;
    const auto q = stride == 1
                       ? load4(input + j)
                       : _mm256_set_pd(input[index + 3 * stride],
                                       input[index + 2 * stride],
                                       input[index + stride], input[index]);
    auto u = _mm256_mul_pd(_mm256_sub_pd(q, first), factor);
    if (lut.edge == fast_curve::Edge::Linear &&
        _mm256_movemask_pd(eq(_mm256_andnot_pd(set(-0.0), u), set(HUGE_VAL)))) {
      for (unsigned k = 0; k < 4; ++k) {
        const auto p =
            lut.position(input[static_cast<std::ptrdiff_t>(j + k) * stride]);
        for (unsigned c = 0; c < Channels; ++c)
          output[(j + k) * Channels + c] = static_cast<Output>(lut.value(p, c));
      }
      continue;
    }
    if (lut.edge != fast_curve::Edge::Linear)
      u = _mm256_min_pd(limit, _mm256_max_pd(zero, u));
    const auto position =
        _mm256_min_pd(cap, _mm256_max_pd(zero, _mm256_floor_pd(u)));
    const auto row = _mm256_cvtepi32_epi64(_mm256_cvttpd_epi32(position));
    const auto offset = _mm256_mul_epi32(row, _mm256_set1_epi64x(Channels));
    const auto next = _mm256_add_epi64(offset, _mm256_set1_epi64x(Channels));
    const auto t = _mm256_sub_pd(u, position);
    const auto at_left = eq(t, zero), at_right = eq(t, one);
    const auto at_first = eq(q, first), at_last = eq(q, last);
    std::array<V, Channels> values;
    const auto interpolate = [&](V a, V b, unsigned c) {
      auto value = _mm256_fmadd_pd(t, _mm256_sub_pd(b, a), a);
      value = choose(at_left, a, value);
      value = choose(at_right, b, value);
      value = choose(at_first, set(lut.table[c]), value);
      return choose(at_last, set(lut.table[(lut.length - 1) * Channels + c]),
                    value);
    };
    // Ordered queries often share one interval. Broadcast its two rows while
    // retaining each query's interpolation fraction and endpoint selections.
    bool shared = false;
    if constexpr (SharedRows) {
      const auto first_offset = _mm256_permute4x64_epi64(offset, 0);
      const auto same_row = _mm256_cmpeq_epi64(offset, first_offset);
      shared = _mm256_movemask_pd(_mm256_castsi256_pd(same_row)) == 15;
    }
    if (shared) {
      const auto base =
          static_cast<std::size_t>(_mm256_extract_epi64(offset, 0));
      for (unsigned c = 0; c < Channels; ++c)
        values[c] = interpolate(set(lut.table[base + c]),
                                set(lut.table[base + Channels + c]), c);
    } else {
      for (unsigned c = 0; c < Channels; ++c)
        values[c] = interpolate(_mm256_i64gather_pd(lut.table + c, offset, 8),
                                _mm256_i64gather_pd(lut.table + c, next, 8), c);
    }
    if constexpr (Channels == 4)
      store_four_columns(output + j * Channels, values);
    else
      store_columns<Output, Channels>(output + j * Channels, values);
  }
  for (; j < count; ++j) {
    const auto position =
        lut.position(input[static_cast<std::ptrdiff_t>(j) * stride]);
    for (unsigned c = 0; c < Channels; ++c)
      output[j * Channels + c] = static_cast<Output>(lut.value(position, c));
  }
}
template <unsigned Channels, class Input, class Output>
void vector_lut_t(const fast_curve::VectorLut& lut, const Input* input,
                  Output* output, std::size_t count, std::ptrdiff_t stride) {
  if (lut.length == 1) {
    if (count * Channels * sizeof(Output) >= 8388608) {
      constant_stream<Channels>(lut.table, output, count);
      return;
    }
    constexpr unsigned lanes = 32 / sizeof(Output);
    alignas(32) Output pattern[Channels * lanes];
    for (unsigned c = 0; c < Channels; ++c)
      pattern[c] = static_cast<Output>(lut.table[c]);
    for (unsigned j = Channels; j < Channels * lanes; ++j)
      pattern[j] = pattern[j % Channels];
    std::size_t j = 0;
    if constexpr (sizeof(Output) == 4) {
      std::array<__m256, Channels> values;
      for (unsigned c = 0; c < Channels; ++c) {
        values[c] = _mm256_load_ps(pattern + c * lanes);
        // Preserve full-width cached stores under strict floating semantics.
        __asm__ __volatile__("" : "+x"(values[c]));
      }
      for (; j + lanes <= count; j += lanes)
        for (unsigned c = 0; c < Channels; ++c)
          _mm256_storeu_ps(output + j * Channels + c * lanes, values[c]);
    } else {
      std::array<V, Channels> values;
      for (unsigned c = 0; c < Channels; ++c)
        values[c] = _mm256_load_pd(pattern + c * lanes);
      for (; j + lanes <= count; j += lanes)
        for (unsigned c = 0; c < Channels; ++c)
          _mm256_storeu_pd(output + j * Channels + c * lanes, values[c]);
    }
    for (; j < count; ++j)
      std::copy_n(pattern, Channels, output + j * Channels);
    return;
  }
  if (!std::isfinite(lut.scale) || lut.scale == 0) {
    lut.generate(
        [&](std::size_t j) {
          return input[static_cast<std::ptrdiff_t>(j) * stride];
        },
        output, count);
    return;
  }
  // A small locality hint selects the loop once per span. The shared-row
  // loop still checks every packet; later changes in query order are safe.
  bool shared_rows = count >= 64;
  if (shared_rows) {
    // Probe the middle too: a flat border need not describe the image body.
    const std::array<std::size_t, 2> probes{0, (count / 2) & ~std::size_t(3)};
    for (auto probe : probes) {
      const auto at = static_cast<std::ptrdiff_t>(probe) * stride;
      const auto row = std::min(lut.length - 2, lut.position(input[at]).row);
      for (std::ptrdiff_t k = 1; k < 4; ++k) {
        if (std::min(lut.length - 2,
                     lut.position(input[at + k * stride]).row) != row) {
          shared_rows = false;
          break;
        }
      }
      if (!shared_rows)
        break;
    }
  }
  if (shared_rows)
    vector_lut_rows<Channels, true>(lut, input, output, count, stride);
  else
    vector_lut_rows<Channels, false>(lut, input, output, count, stride);
}
template <class Input, class Output>
void vector_lut_dispatch(const fast_curve::VectorLut& lut, const void* source,
                         void* target, std::size_t count,
                         std::ptrdiff_t stride) {
  const auto* input = static_cast<const Input*>(source);
  auto* output = static_cast<Output*>(target);
  if (lut.columns == 3) {
    vector_lut_t<3>(lut, input, output, count, stride);
  } else if (lut.columns == 4) {
    vector_lut_t<4>(lut, input, output, count, stride);
  } else if (lut.columns == 1) {
    vector_lut_t<1>(lut, input, output, count, stride);
  } else {
    lut.generate(
        [&](std::size_t j) {
          return input[static_cast<std::ptrdiff_t>(j) * stride];
        },
        output, count);
  }
}
template <class T>
std::array<V, 3> gradient_rgb(const T* source, unsigned channels) {
  if (channels == 3) {
    if constexpr (sizeof(T) == 8) {
      return fast_color::avx2::Packet<double>::load3(source);
    } else {
      const auto rgb = fast_color::Packet<float>::load3(source);
      return {_mm256_cvtps_pd(rgb[0]), _mm256_cvtps_pd(rgb[1]),
              _mm256_cvtps_pd(rgb[2])};
    }
  }
  if constexpr (sizeof(T) == 4) {
    auto a = _mm_loadu_ps(source), b = _mm_loadu_ps(source + 4);
    auto c = _mm_loadu_ps(source + 8), d = _mm_loadu_ps(source + 12);
    _MM_TRANSPOSE4_PS(a, b, c, d);
    return {_mm256_cvtps_pd(a), _mm256_cvtps_pd(b), _mm256_cvtps_pd(c)};
  } else {
    const auto a = _mm256_loadu_pd(source), b = _mm256_loadu_pd(source + 4);
    const auto c = _mm256_loadu_pd(source + 8),
               d = _mm256_loadu_pd(source + 12);
    const auto t0 = _mm256_unpacklo_pd(a, b), t1 = _mm256_unpackhi_pd(a, b);
    const auto t2 = _mm256_unpacklo_pd(c, d), t3 = _mm256_unpackhi_pd(c, d);
    return {_mm256_permute2f128_pd(t0, t2, 0x20),
            _mm256_permute2f128_pd(t1, t3, 0x20),
            _mm256_permute2f128_pd(t0, t2, 0x31)};
  }
}
inline V gradient_dot(const fast_color::Triple& row,
                      const std::array<V, 3>& v) {
  return _mm256_fmadd_pd(
      v[2], set(row[2]),
      _mm256_fmadd_pd(v[1], set(row[1]), _mm256_mul_pd(v[0], set(row[0]))));
}
template <unsigned Mode, class T>
void gradient_map_t(unsigned channels, const grade::gradient::Feature& feature,
                    const fast_curve::Curve* curves, const void* source,
                    void* target, std::size_t count) {
  const auto* input = static_cast<const T*>(source);
  auto* output = static_cast<T*>(target);
  constexpr std::size_t block = 256;
  alignas(64) double args[3][block], roots[3][block], query[block];
  alignas(64) T rgb_output[3 * block];
  for (std::size_t base = 0; base < count; base += block) {
    const auto n = std::min(block, count - base);
    std::size_t j = 0;
    for (; j + 4 <= n; j += 4) {
      const auto rgb = gradient_rgb(input + (base + j) * channels, channels);
      if constexpr (Mode == 0) {
        _mm256_store_pd(query + j,
                        _mm256_max_pd(rgb[0], _mm256_max_pd(rgb[1], rgb[2])));
      } else if constexpr (Mode == 1) {
        _mm256_store_pd(query + j, gradient_dot(feature.rgb_to_xyz[1], rgb));
      } else if constexpr (Mode == 2) {
        _mm256_store_pd(args[0] + j, gradient_dot(feature.rgb_to_xyz[1], rgb));
      } else {
        for (unsigned c = 0; c < 3; ++c)
          _mm256_store_pd(args[c] + j,
                          gradient_dot(feature.rgb_to_lms[c], rgb));
      }
    }
    for (; j < n; ++j) {
      const auto* pixel = input + (base + j) * channels;
      const fast_color::Triple rgb{pixel[0], pixel[1], pixel[2]};
      if constexpr (Mode == 0) {
        query[j] = std::max(rgb[0], std::max(rgb[1], rgb[2]));
      } else if constexpr (Mode == 1) {
        query[j] = fast_color::dot(feature.rgb_to_xyz[1], rgb);
      } else if constexpr (Mode == 2) {
        args[0][j] = fast_color::dot(feature.rgb_to_xyz[1], rgb);
      } else {
        const auto lms = fast_color::multiply(feature.rgb_to_lms, rgb);
        for (unsigned c = 0; c < 3; ++c)
          args[c][j] = lms[c];
      }
    }
    if constexpr (Mode >= 2) {
      for (unsigned c = 0; c < (Mode == 2 ? 1U : 3U); ++c)
        cube_roots_refined(args[c], roots[c], n, 2);
      j = 0;
      for (; j + 4 <= n; j += 4) {
        V value;
        if constexpr (Mode == 2) {
          const auto y = _mm256_load_pd(args[0] + j);
          const auto high = _mm256_div_pd(
              _mm256_sub_pd(
                  _mm256_mul_pd(_mm256_load_pd(roots[0] + j), set(116)),
                  set(16)),
              set(100));
          const auto low =
              _mm256_div_pd(_mm256_mul_pd(y, set(24389.0 / 27)), set(100));
          value = choose(le(y, set(216.0 / 24389)), low, high);
        } else {
          value = gradient_dot(
              fast_color::kLmsToOklab[0],
              {_mm256_load_pd(roots[0] + j), _mm256_load_pd(roots[1] + j),
               _mm256_load_pd(roots[2] + j)});
        }
        _mm256_store_pd(query + j, value);
      }
      for (; j < n; ++j) {
        if constexpr (Mode == 2)
          query[j] = args[0][j] > 216.0 / 24389
                         ? (116 * roots[0][j] - 16) / 100
                         : (24389.0 / 27) * args[0][j] / 100;
        else
          query[j] = fast_color::dot(fast_color::kLmsToOklab[0],
                                     {roots[0][j], roots[1][j], roots[2][j]});
      }
    }
    auto* destination = channels == 3 ? output + base * 3 : rgb_output;
    curve_t<double, T>(curves, 3, query, destination, n);
    if (channels == 4) {
      for (std::size_t j = 0; j < n; ++j) {
        std::memcpy(output + (base + j) * 4, rgb_output + j * 3, 3 * sizeof(T));
        std::memcpy(output + (base + j) * 4 + 3, input + (base + j) * 4 + 3,
                    sizeof(T));
      }
    }
  }
}
template <class T>
void gradient_map_dispatch(unsigned channels,
                           const grade::gradient::Feature& feature,
                           const fast_curve::Curve* curves, const void* input,
                           void* output, std::size_t count) {
  switch (feature.mode) {
    case 0:
      gradient_map_t<0, T>(channels, feature, curves, input, output, count);
      break;
    case 1:
      gradient_map_t<1, T>(channels, feature, curves, input, output, count);
      break;
    case 2:
      gradient_map_t<2, T>(channels, feature, curves, input, output, count);
      break;
    default:
      gradient_map_t<3, T>(channels, feature, curves, input, output, count);
      break;
  }
}
}  // namespace
namespace {
void cube_roots_refined(const double* input, double* output, std::size_t count,
                        unsigned iterations) {
  std::size_t j = 0;
  for (; j + 4 <= count; j += 4) {
    std::array<double, 4> normalized, scales;
    bool ordinary = true;
    for (unsigned c = 0; c < 4; ++c) {
      std::uint64_t bits;
      std::memcpy(&bits, input + j + c, 8);
      const unsigned exponent = (bits >> 52) & 2047;
      if (!exponent || exponent == 2047) {
        ordinary = false;
        break;
      }
      const int e = static_cast<int>(exponent) - 1023;
      const int q = e >= 0 ? e / 3 : (e - 2) / 3;
      const int remainder = e - 3 * q;
      const auto mantissa =
          (bits & UINT64_C(0xfffffffffffff)) |
          (static_cast<std::uint64_t>(1023 + remainder) << 52);
      const auto scale = (bits & UINT64_C(0x8000000000000000)) |
                         (static_cast<std::uint64_t>(1023 + q) << 52);
      std::memcpy(&normalized[c], &mantissa, 8);
      std::memcpy(&scales[c], &scale, 8);
    }
    if (!ordinary) {
      for (unsigned c = 0; c < 4; ++c) {
        output[j + c] = std::cbrt(input[j + c]);
      }
      continue;
    }
    const auto x = _mm256_loadu_pd(normalized.data());
    auto y =
        _mm256_fmadd_pd(_mm256_fmadd_pd(set(-72.0 / 4921), x, set(193.0 / 703)),
                        x, set(3642.0 / 4921));
    // Exponent reduction gives x in [1,8). Three Halley refinements from
    // this bounded quadratic seed converge to native binary64 accuracy.
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
      const auto cube = _mm256_mul_pd(_mm256_mul_pd(y, y), y);
      y = _mm256_mul_pd(y, _mm256_div_pd(_mm256_fmadd_pd(set(2), x, cube),
                                         _mm256_fmadd_pd(set(2), cube, x)));
    }
    _mm256_storeu_pd(output + j,
                     _mm256_mul_pd(y, _mm256_loadu_pd(scales.data())));
  }
  for (; j < count; ++j) {
    output[j] = std::cbrt(input[j]);
  }
}
}  // namespace
void cube_roots(const double* input, double* output, std::size_t count) {
  cube_roots_refined(input, output, count, 3);
}
void model(unsigned kind, bool narrow, const fast_color::Model& m,
           const void* x, void* y, std::size_t n, std::size_t ic,
           std::size_t oc, const std::array<std::uint64_t, 3>& selected) {
  if (narrow) {
    model_dispatch<float>(kind, m, x, y, n, ic, oc, selected);
  } else {
    model_dispatch<double>(kind, m, x, y, n, ic, oc, selected);
  }
}
void matrix(bool narrow, const fast_color::Matrix& m, const void* x, void* y,
            std::size_t n) {
  if (narrow) {
    matrix_t<float>(m, x, y, n);
  } else {
    matrix_t<double>(m, x, y, n);
  }
}
void curve(bool input32, bool output32, const fast_curve::Curve* curves,
           unsigned columns, const void* input, void* output, std::size_t n) {
  if (input32) {
    if (output32) {
      curve_t<float, float>(curves, columns, input, output, n);
    } else {
      curve_t<float, double>(curves, columns, input, output, n);
    }
  } else {
    if (output32) {
      curve_t<double, float>(curves, columns, input, output, n);
    } else {
      curve_t<double, double>(curves, columns, input, output, n);
    }
  }
}
void lut(bool input32, bool output32, const void* input, void* output,
         std::size_t n, const double* values, std::size_t length, double start,
         double end, unsigned edge, std::ptrdiff_t input_step,
         std::ptrdiff_t output_step) {
#define PS_LUT(I, O)                                        \
  lut_t<I, O>(input, output, n, values, length, start, end, \
              static_cast<fast_curve::Edge>(edge), input_step, output_step)
  if (input32) {
    if (output32) {
      PS_LUT(float, float);
    } else {
      PS_LUT(float, double);
    }
  } else {
    if (output32) {
      PS_LUT(double, float);
    } else {
      PS_LUT(double, double);
    }
  }
#undef PS_LUT
}
void polynomial(bool narrow, const fast_sampling::Polynomial& p, double start,
                double end, std::uint64_t total, std::uint64_t first,
                void* output, std::size_t n) {
  if (narrow) {
    polynomial_dispatch<float>(p, start, end, total, first, output, n);
  } else {
    polynomial_dispatch<double>(p, start, end, total, first, output, n);
  }
}
void coordinates(bool narrow, double start, double end, std::uint64_t total,
                 std::uint64_t first, void* output, std::size_t n) {
  fast_sampling::Polynomial p;
  p.degree = 1;
  p.coefficients[1] = 1;
  if (std::isfinite(end - start)) {
    polynomial(narrow, p, start, end, total, first, output, n);
  } else {
    for (std::size_t j = 0; j < n; ++j) {
      const auto q = fast_sampling::coordinate(start, end, first + j, total);
      if (narrow) {
        static_cast<float*>(output)[j] = static_cast<float>(q);
      } else {
        static_cast<double*>(output)[j] = q;
      }
    }
  }
  if (n && first == 0) {
    if (narrow) {
      static_cast<float*>(output)[0] = static_cast<float>(start);
    } else {
      static_cast<double*>(output)[0] = start;
    }
  }
  if (n && first + n == total) {
    if (narrow) {
      static_cast<float*>(output)[n - 1] = static_cast<float>(end);
    } else {
      static_cast<double*>(output)[n - 1] = end;
    }
  }
}
void brightness(bool narrow, unsigned channels,
                const grade::curves::Brightness& b,
                const fast_curve::Curve& curve, const void* x, void* y,
                std::size_t n) {
  if (narrow) {
    brightness_t<float>(channels, b, curve, x, y, n);
  } else {
    brightness_t<double>(channels, b, curve, x, y, n);
  }
}
void vector_lut(bool input32, bool output32, const fast_curve::VectorLut& lut,
                const void* input, void* output, std::size_t count,
                std::ptrdiff_t stride) {
  if (input32) {
    if (output32)
      vector_lut_dispatch<float, float>(lut, input, output, count, stride);
    else
      vector_lut_dispatch<float, double>(lut, input, output, count, stride);
  } else {
    if (output32)
      vector_lut_dispatch<double, float>(lut, input, output, count, stride);
    else
      vector_lut_dispatch<double, double>(lut, input, output, count, stride);
  }
}
void gradient_map(bool narrow, unsigned channels,
                  const grade::gradient::Feature& feature,
                  const fast_curve::Curve* curves, const void* input,
                  void* output, std::size_t count) {
  if (narrow)
    gradient_map_dispatch<float>(channels, feature, curves, input, output,
                                 count);
  else
    gradient_map_dispatch<double>(channels, feature, curves, input, output,
                                  count);
}
void components(bool narrow, unsigned channels, unsigned selected,
                const fast_curve::Curve* curves, const void* x, void* y,
                std::size_t n) {
  if (narrow) {
    components_t<float>(channels, selected, curves, x, y, n);
  } else {
    components_t<double>(channels, selected, curves, x, y, n);
  }
}
}  // namespace ps::plugin_internal::native_compute
