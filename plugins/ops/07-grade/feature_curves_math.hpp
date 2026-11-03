// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "01-numeric/fast_curve.hpp"
#include "01-numeric/fast_native_simd.hpp"
#include "02-format-color/fast_color.hpp"
#include "07-grade/affine_math.hpp"
#include "07-grade/curves_math.hpp"

namespace ps::plugin_internal::grade::feature {
// A/B use invocation-owned tables. The seam is an ordinary final segment.
inline double wrap(double value, double period) {
  const double quotient = value / period;
  double result;
  if (std::abs(quotient) < 1048576) {
    result = std::fma(-std::floor(quotient), period, value);
  } else {
    result = std::fmod(value, period);
  }
  if (result < 0)
    result += period;
  if (result >= period)
    result = 0;
  return result;
}

// Local interval identities have identical semantics in dense and sparse
// table access; exact knots always select their stored ordinate.
inline double linear_value(double q, double x0, double x1, double y0, double y1,
                           bool nonnegative = false) {
  if (q == x0)
    return y0;
  if (q == x1)
    return y1;
  if (x0 == y0 && x1 == y1)
    return q == 0 ? 0.0 : q;
  if (y0 == y1)
    return y0 == 0 ? 0.0 : y0;
  const double slope = (y1 - y0) / (x1 - x0);
  // Scale fields anchor at the smaller ordinate so interpolation cannot
  // cancel across zero. Signed modulation retains its left-endpoint profile.
  return nonnegative && y1 < y0 ? std::fma(q - x1, slope, y1)
                                : std::fma(q - x0, slope, y0);
}
template <class X, class Y>
inline double sparse_value(double q, std::size_t size, double period, X x, Y y,
                           bool nonnegative = false) {
  if (period)
    q = wrap(q, period);
  const double first = x(0), last = x(size - 1);
  if (q <= first)
    return y(0);
  if (q >= last) {
    if (!period || q == last)
      return y(size - 1);
    return linear_value(q, last, period, y(size - 1), y(0), nonnegative);
  }
  std::size_t lo = 0, hi = size - 1;
  // An interpolation hint is only accepted after checking both true knots.
  // Irregular and extreme-width tables retain the same binary search.
  const double width = last - first;
  if (std::isfinite(width) && width > 0) {
    const double position = (q - first) / width * (size - 1);
    const auto guess = static_cast<std::size_t>(
        std::clamp(position, 0.0, static_cast<double>(size - 2)));
    const double x0 = x(guess), x1 = x(guess + 1);
    if (q >= x0 && q <= x1)
      return linear_value(q, x0, x1, y(guess), y(guess + 1), nonnegative);
    if (q < x0)
      hi = guess;
    else
      lo = guess + 1;
  }
  while (lo + 1 < hi) {
    const auto mid = lo + (hi - lo) / 2;
    if (q < x(mid))
      hi = mid;
    else
      lo = mid;
  }
  return linear_value(q, x(lo), x(hi), y(lo), y(hi), nonnegative);
}

struct Curve {
  fast_curve::Curve curve;
  double period = 0;
  bool nonnegative = false;
  std::size_t guide_distance = 0;
  double uniform_scale = 0;
  void prepare(const double* knots, const double* values, std::size_t count,
               fast_curve::Edge edge, double* workspace) {
    curve.prepare(knots, values, count, false, edge, workspace);
    guide_distance = 0;
    uniform_scale = 0;
    const double width = knots[count - 1] - knots[0];
    const double step = width / (count - 1);
    if (count > 2 && std::isfinite(width) && step > 0 &&
        std::isfinite(1 / step)) {
      bool uniform = true;
      for (std::size_t j = 1; j + 1 < count; ++j)
        uniform &= std::abs(knots[j] - std::fma(static_cast<double>(j), step,
                                                knots[0])) <= step * 1e-12;
      if (uniform)
        uniform_scale = (count - 1) / width;
    }
    if (count >= 64 && std::isfinite(curve.index_scale) &&
        curve.index_scale > 0) {
      for (unsigned j = 0; j < curve.guide_count; ++j)
        guide_distance = std::max<std::size_t>(
            guide_distance, curve.guide[j + 1] + 1 - curve.guide[j]);
    }
  }
  double evaluate(double value) const {
    const double q = period ? wrap(value, period) : value;
    if (q <= curve.x[0])
      return curve.y[0];
    if (q >= curve.x[curve.size - 1])
      return curve.y[curve.size - 1];
    if (curve.identity || curve.constant)
      return curve.evaluate(q);
    const auto i = curve.segment(q);
    if (q == curve.x[i])
      return curve.y[i];
    if (curve.x[i] == curve.y[i] && curve.x[i + 1] == curve.y[i + 1])
      return q == 0 ? 0.0 : q;
    if (curve.y[i] == curve.y[i + 1])
      return curve.y[i] == 0 ? 0.0 : curve.y[i];
    const auto anchor = nonnegative && curve.y[i + 1] < curve.y[i] ? i + 1 : i;
    return std::fma(q - curve.x[anchor], curve.a[i], curve.y[anchor]);
  }
  template <class T>
  void span(const T* input, double* output, std::size_t count) const;
};

/** Per-sample native edits. RGB routes use published binary64 features. */
struct Edit {
  unsigned member = 2;
  unsigned hue = 0;
  curves::Brightness brightness;
  bool rgb() const { return member == 5 || member >= 7; }
  bool active(unsigned component) const {
    if (rgb())
      return true;
    if (member == 2)
      return component == hue;
    if (member == 3)
      return component != 0;
    return component == (member == 4 ? 0U : 1U);
  }
  bool copies(double field, double gate = 1) const {
    if (member == 2)
      return gate == 0 || field == 0;
    return member == 4 ? field == 0 : field == 1;
  }
  double coordinate(double value, double field) const {
    if (member == 2 || member == 4)
      return value + field;
    const double y = value * field;
    return member == 6 ? std::clamp(y, 0.0, 1.0) : y;
  }
  fast_color::Triple replace(const fast_color::Triple& x, double target,
                             bool* copy) const {
    const auto& b = brightness;
    if (member == 5 || member == 7) {
      const double source = fast_color::dot(b.forward[1], x);
      *copy = target == source;
      if (*copy)
        return x;
      if (member == 5) {
        if (source == 0)
          return {target, target, target};
        const double ratio = target / source;
        return {x[0] * ratio, x[1] * ratio, x[2] * ratio};
      }
      const double delta = target - source;
      return {std::fma(b.inverse[0][1], delta, x[0]),
              std::fma(b.inverse[1][1], delta, x[1]),
              std::fma(b.inverse[2][1], delta, x[2])};
    }
    if (member == 8) {
      auto xyz = fast_color::multiply(b.forward, x);
      auto lab = fast_color::xyz_to_lab(xyz, b.white);
      *copy = target == lab[0];
      if (*copy)
        return x;
      lab[0] = target;
      return fast_color::multiply(b.inverse,
                                  fast_color::lab_to_xyz(lab, b.white));
    }
    auto roots = fast_color::multiply(b.rgb_to_lms, x);
    for (auto& v : roots)
      v = std::cbrt(v);
    const double source = fast_color::dot(fast_color::kLmsToOklab[0], roots);
    *copy = target == source;
    if (*copy)
      return x;
    const double delta = target - source;
    fast_color::Triple cubes;
    for (unsigned c = 0; c < 3; ++c) {
      const double r = roots[c], t = b.lightness_direction[c] * delta;
      cubes[c] = t * std::fma(t, t, std::fma(3 * r, t, 3 * r * r));
    }
    const auto correction = fast_color::multiply(b.lms_to_rgb, cubes);
    return {x[0] + correction[0], x[1] + correction[1], x[2] + correction[2]};
  }
  template <class T, unsigned C>
  void scalar(const T* input, const double* field, T* output,
              std::size_t count) const {
    for (std::size_t i = 0; i < count; ++i) {
      const auto* x = input + C * i;
      auto* y = output + C * i;
      if (rgb()) {
        bool copy;
        const auto z = replace({x[0], x[1], x[2]}, field[i], &copy);
        for (unsigned c = 0; c < C; ++c) {
          if (c >= 3 || copy)
            std::memcpy(y + c, x + c, sizeof(T));
          else
            y[c] = static_cast<T>(z[c]);
        }
      } else {
        const bool copy = copies(field[i], x[1]);
        for (unsigned c = 0; c < C; ++c) {
          if (c >= 3 || !active(c) || copy)
            std::memcpy(y + c, x + c, sizeof(T));
          else
            y[c] = static_cast<T>(coordinate(x[c], field[i]));
        }
      }
    }
  }
  template <class T, unsigned C>
  void span(const T* input, const double* field, T* output,
            std::size_t count) const;
};

#if defined(__aarch64__) || defined(__x86_64__)
#if defined(__x86_64__)
#define PS_FEATURE_TARGET __attribute__((target("avx2,fma")))
#else
#define PS_FEATURE_TARGET
#endif
namespace v = affine;
PS_FEATURE_TARGET inline v::Mask ge(v::Packet a, v::Packet b) {
#if defined(__aarch64__)
  return native_simd::greater_equal(a, b);
#else
  return v::ge(a, b);
#endif
}
PS_FEATURE_TARGET inline v::Mask le(v::Packet a, v::Packet b) {
  return ge(b, a);
}
PS_FEATURE_TARGET inline v::Mask greater(v::Packet a, v::Packet b) {
#if defined(__aarch64__)
  return native_simd::greater(a, b);
#else
  return _mm256_cmp_pd(a, b, _CMP_GT_OQ);
#endif
}
PS_FEATURE_TARGET inline v::Mask equal(v::Packet a, v::Packet b) {
#if defined(__aarch64__)
  return native_simd::equal(a, b);
#else
  return _mm256_cmp_pd(a, b, _CMP_EQ_OQ);
#endif
}
PS_FEATURE_TARGET inline v::Mask either(v::Mask a, v::Mask b) {
#if defined(__aarch64__)
  return vorrq_u64(a, b);
#else
  return _mm256_or_pd(a, b);
#endif
}
PS_FEATURE_TARGET inline v::Packet divide(v::Packet a, v::Packet b) {
#if defined(__aarch64__)
  return vdivq_f64(a, b);
#else
  return _mm256_div_pd(a, b);
#endif
}
PS_FEATURE_TARGET inline v::Packet floor(v::Packet a) {
#if defined(__aarch64__)
  return vrndmq_f64(a);
#else
  return _mm256_floor_pd(a);
#endif
}
PS_FEATURE_TARGET inline v::Packet wrapped(v::Packet x, double period) {
  const auto p = v::splat(period), q = divide(x, p);
  if (!v::all(le(v::absolute(q), v::splat(1048575)))) {
    double a[v::kLanes];
    v::save(a, x);
    for (auto& value : a)
      value = wrap(value, period);
    return v::load(a);
  }
  auto y = v::madd(v::sub(v::splat(0), floor(q)), p, x);
  y = v::choose(ge(y, v::splat(0)), y, v::add(y, p));
  return v::choose(ge(y, p), v::splat(0), y);
}
#if defined(__aarch64__)
using IndexPacket = uint64x2_t;
PS_FEATURE_TARGET inline IndexPacket index_splat(std::uint64_t n) {
  return vdupq_n_u64(n);
}
PS_FEATURE_TARGET inline IndexPacket index_add(IndexPacket a, IndexPacket b) {
  return vaddq_u64(a, b);
}
PS_FEATURE_TARGET inline IndexPacket midpoint(IndexPacket a, IndexPacket b) {
  return vshrq_n_u64(vaddq_u64(a, b), 1);
}
PS_FEATURE_TARGET inline IndexPacket index_choose(v::Mask m, IndexPacket a,
                                                  IndexPacket b) {
  return vbslq_u64(m, a, b);
}
PS_FEATURE_TARGET inline v::Packet gather(const double* a, IndexPacket i) {
  return vcombine_f64(vld1_f64(a + vgetq_lane_u64(i, 0)),
                      vld1_f64(a + vgetq_lane_u64(i, 1)));
}
#else
using IndexPacket = __m256i;
PS_FEATURE_TARGET inline IndexPacket index_splat(std::uint64_t n) {
  return _mm256_set1_epi64x(n);
}
PS_FEATURE_TARGET inline IndexPacket index_add(IndexPacket a, IndexPacket b) {
  return _mm256_add_epi64(a, b);
}
PS_FEATURE_TARGET inline IndexPacket midpoint(IndexPacket a, IndexPacket b) {
  return _mm256_srli_epi64(_mm256_add_epi64(a, b), 1);
}
PS_FEATURE_TARGET inline IndexPacket index_choose(v::Mask m, IndexPacket a,
                                                  IndexPacket b) {
  return _mm256_blendv_epi8(b, a, _mm256_castpd_si256(m));
}
PS_FEATURE_TARGET inline v::Packet gather(const double* a, IndexPacket i) {
  return _mm256_i64gather_pd(a, i, 8);
}
#endif
PS_FEATURE_TARGET inline IndexPacket index_truncate(v::Packet p) {
#if defined(__aarch64__)
  return vcvtq_u64_f64(p);
#else
  return _mm256_cvtepu32_epi64(_mm256_cvttpd_epi32(p));
#endif
}
PS_FEATURE_TARGET inline IndexPacket guide_gather(const std::uint32_t* a,
                                                  IndexPacket i) {
#if defined(__aarch64__)
  auto result = vdupq_n_u64(a[vgetq_lane_u64(i, 0)]);
  return vsetq_lane_u64(a[vgetq_lane_u64(i, 1)], result, 1);
#else
  return _mm256_cvtepu32_epi64(
      _mm256_i64gather_epi32(reinterpret_cast<const int*>(a), i, 4));
#endif
}
template <class T, bool Nonnegative>
PS_FEATURE_TARGET inline std::size_t curve_packets(const Curve& op,
                                                   const T* input,
                                                   double* output,
                                                   std::size_t count) {
  const auto& c = op.curve;
  if (c.identity || c.constant)
    return 0;
  const auto first = v::splat(c.x[0]), last = v::splat(c.x[c.size - 1]);
  const auto yfirst = v::splat(c.y[0]), ylast = v::splat(c.y[c.size - 1]);
  std::size_t i = 0;
  for (; i + v::kLanes <= count; i += v::kLanes) {
    auto q = v::load(input + i);
    if (op.period)
      q = wrapped(q, op.period);
    const auto low = le(q, first), high = ge(q, last);
    if (v::all(either(low, high))) {
      v::save(output + i, v::choose(low, yfirst, ylast));
      continue;
    }
    q = v::choose(low, first, v::choose(high, last, q));
    auto lo = index_splat(0), hi = index_splat(c.size - 1);
    std::size_t distance = c.size - 1;
    if (op.uniform_scale) {
      auto position = v::mul(v::sub(q, first), v::splat(op.uniform_scale));
      const auto bound = v::splat(c.size - 2);
      position = v::choose(ge(position, bound), bound, position);
      lo = index_truncate(position);
      hi = index_add(lo, index_splat(1));
      const auto valid = v::both(ge(q, gather(c.x, lo)),
                                 either(high, greater(gather(c.x, hi), q)));
      if (v::all(valid)) {
        distance = 1;
      } else {
        lo = index_choose(valid, lo, index_splat(0));
        hi = index_choose(valid, hi, index_splat(c.size - 1));
      }
    } else if (op.guide_distance) {
      auto position = v::mul(v::sub(q, first), v::splat(c.index_scale));
      const auto bound = v::splat(c.guide_count - 1);
      position = v::choose(ge(position, bound), bound, position);
      const auto bucket = index_truncate(position);
      lo = guide_gather(c.guide.data(), bucket);
      hi = index_add(
          guide_gather(c.guide.data(), index_add(bucket, index_splat(1))),
          index_splat(1));
      const auto valid = v::both(ge(q, gather(c.x, lo)),
                                 either(high, greater(gather(c.x, hi), q)));
      if (v::all(valid)) {
        distance = op.guide_distance;
      } else {
        lo = index_choose(valid, lo, index_splat(0));
        hi = index_choose(valid, hi, index_splat(c.size - 1));
      }
    }
    for (; distance > 1; distance = (distance + 1) / 2) {
      const auto mid = midpoint(lo, hi);
      // Equality belongs to the interval beginning at this exact knot.
#if defined(__aarch64__)
      const auto strictly_less = native_simd::greater(gather(c.x, mid), q);
#else
      const auto strictly_less = _mm256_cmp_pd(q, gather(c.x, mid), _CMP_LT_OQ);
#endif
      hi = index_choose(strictly_less, mid, hi);
      lo = index_choose(strictly_less, lo, mid);
    }
    const auto upper = index_add(lo, index_splat(1));
    const auto x0 = gather(c.x, lo), x1 = gather(c.x, upper);
    const auto y0 = gather(c.y, lo), y1 = gather(c.y, upper);
    auto anchor_x = x0, anchor_y = y0;
    if constexpr (Nonnegative) {
      const auto descending = greater(y0, y1);
      anchor_x = v::choose(descending, x1, x0);
      anchor_y = v::choose(descending, y1, y0);
    }
    auto y = v::madd(v::sub(q, anchor_x), gather(c.a, lo), anchor_y);
    const auto identity = v::both(equal(x0, y0), equal(x1, y1));
    y = v::choose(identity, v::choose(equal(q, v::splat(0)), v::splat(0), q),
                  y);
    y = v::choose(equal(y0, y1),
                  v::choose(equal(y0, v::splat(0)), v::splat(0), y0), y);
    y = v::choose(equal(q, x0), y0, y);
    y = v::choose(low, yfirst, v::choose(high, ylast, y));
    v::save(output + i, y);
  }
  return i;
}

// Gathering only a bounded packet also handles RGB/RGBA without alignment
// assumptions. Saves select original stored bits before converting bypasses.
template <unsigned Member, class T, unsigned C>
PS_FEATURE_TARGET inline std::size_t edit_packets(const Edit& op,
                                                  const T* input,
                                                  const double* field,
                                                  T* output,
                                                  std::size_t count) {
  if constexpr (Member >= 8)
    return 0;
  std::size_t i = 0;
  const auto zero = v::splat(0), one = v::splat(1);
  for (; i + v::kLanes <= count; i += v::kLanes) {
    T source[C][v::kLanes], target[C][v::kLanes];
    for (unsigned c = 0; c < C; ++c)
      for (unsigned j = 0; j < v::kLanes; ++j)
        std::memcpy(&source[c][j], input + C * (i + j) + c, sizeof(T));
    v::Packet x[3], y[3];
    v::Mask copy[3];
    for (unsigned c = 0; c < 3; ++c) {
      const bool needed = Member == 5 || Member == 7 ||
                          (Member == 2   ? c == op.hue || c == 1
                           : Member == 3 ? c != 0
                                         : c == (Member == 4 ? 0U : 1U));
      x[c] = needed ? v::load(source[c]) : zero;
    }
    const auto f = v::load(field + i);
    if constexpr (Member == 5 || Member == 7) {
      const auto& b = op.brightness;
      auto feature = v::mul(v::splat(b.forward[1][0]), x[0]);
      feature = v::madd(v::splat(b.forward[1][1]), x[1], feature);
      feature = v::madd(v::splat(b.forward[1][2]), x[2], feature);
      const auto same = equal(f, feature);
      if (Member == 5) {
        const auto black = equal(feature, zero);
        const auto ratio = divide(f, v::choose(black, one, feature));
        for (unsigned c = 0; c < 3; ++c)
          y[c] = v::choose(black, f, v::mul(x[c], ratio));
      } else {
        const auto delta = v::sub(f, feature);
        for (unsigned c = 0; c < 3; ++c)
          y[c] = v::madd(v::splat(b.inverse[c][1]), delta, x[c]);
      }
      for (auto& mask : copy)
        mask = same;
    } else {
      auto same = equal(f, Member == 2 || Member == 4 ? zero : one);
      if (Member == 2)
        same = either(same, equal(x[1], zero));
      for (unsigned c = 0; c < 3; ++c) {
        const bool active = Member == 2   ? c == op.hue
                            : Member == 3 ? c != 0
                                          : c == (Member == 4 ? 0U : 1U);
        copy[c] = active ? same : equal(zero, zero);
        y[c] = x[c];
        if (active) {
          y[c] = Member == 2 || Member == 4 ? v::add(x[c], f) : v::mul(x[c], f);
          if constexpr (Member == 6) {
            y[c] = v::choose(ge(y[c], zero), y[c], zero);
            y[c] = v::choose(le(y[c], one), y[c], one);
          }
        }
      }
    }
    for (unsigned c = 0; c < C; ++c) {
      const bool changed =
          c < 3 && (Member == 5 || Member == 7 ||
                    (Member == 2   ? c == op.hue
                     : Member == 3 ? c != 0
                                   : c == (Member == 4 ? 0U : 1U)));
      if (changed)
        v::save_preserving(target[c], source[c], y[c], copy[c]);
      else
        std::memcpy(target[c], source[c], sizeof(source[c]));
      for (unsigned j = 0; j < v::kLanes; ++j)
        std::memcpy(output + C * (i + j) + c, &target[c][j], sizeof(T));
    }
  }
  return i;
}
#undef PS_FEATURE_TARGET
#endif

template <class T>
inline void Curve::span(const T* input, double* output,
                        std::size_t count) const {
  std::size_t i = 0;
#if defined(__aarch64__) || defined(__x86_64__)
#if defined(__x86_64__)
  if (native_compute::available())
#endif
    i = nonnegative ? curve_packets<T, true>(*this, input, output, count)
                    : curve_packets<T, false>(*this, input, output, count);
#endif
  for (; i < count; ++i)
    output[i] = evaluate(input[i]);
}
#if defined(__x86_64__)
// The existing native cube-root kernel reduces exponents and performs three
// Halley refinements. Small neighborhoods of identity use the scalar
// extraction sequence so exact copy decisions are independent of batching.
template <unsigned Member, class T, unsigned C>
__attribute__((target("avx2,fma"))) inline void perceptual_span(
    const Edit& op, const T* input, const double* field, T* output,
    std::size_t count) {
  constexpr std::size_t kBlock = 128;
  double values[3 * kBlock], roots[3 * kBlock];
  const auto& b = op.brightness;
  for (std::size_t first = 0; first < count; first += kBlock) {
    const auto n = std::min(kBlock, count - first);
    for (std::size_t j = 0; j < n; ++j) {
      const auto* x = input + C * (first + j);
      auto v = fast_color::multiply(Member == 8 ? b.forward : b.rgb_to_lms,
                                    fast_color::Triple{x[0], x[1], x[2]});
      for (unsigned c = 0; c < 3; ++c)
        values[3 * j + c] = Member == 8 ? v[c] / b.white[c] : v[c];
    }
    native_compute::cube_roots(values, roots, 3 * n);
    for (std::size_t j = 0; j < n; ++j) {
      const auto i = first + j;
      const auto* x = input + C * i;
      auto* out = output + C * i;
      fast_color::Triple r{roots[3 * j], roots[3 * j + 1], roots[3 * j + 2]};
      if (Member == 8) {
        for (unsigned c = 0; c < 3; ++c)
          if (values[3 * j + c] <= fast_color::kEpsilon)
            r[c] = values[3 * j + c] /
                       (3 * fast_color::kDelta * fast_color::kDelta) +
                   4.0 / 29;
      }
      const double source =
          Member == 8 ? 1.16 * r[1] - .16
                      : fast_color::dot(fast_color::kLmsToOklab[0], r);
      bool copy = false;
      fast_color::Triple result;
      // Bound the extraction error before cancellation: signed HDR LMS
      // roots can nearly cancel in L even when their individual terms are
      // large.
      const double scale =
          Member == 8 ? 1.16 * std::abs(r[1]) + .16
                      : std::abs(fast_color::kLmsToOklab[0][0] * r[0]) +
                            std::abs(fast_color::kLmsToOklab[0][1] * r[1]) +
                            std::abs(fast_color::kLmsToOklab[0][2] * r[2]);
      if (std::abs(field[i] - source) <= 0x1p-48 * std::max(1.0, scale)) {
        result = op.replace({x[0], x[1], x[2]}, field[i], &copy);
      } else if (Member == 8) {
        const double delta = (field[i] + .16) / 1.16 - r[1];
        for (unsigned c = 0; c < 3; ++c)
          r[c] = b.white[c] * fast_color::cie_g(r[c] + delta);
        result = fast_color::multiply(b.inverse, r);
      } else {
        const double delta = field[i] - source;
        for (unsigned c = 0; c < 3; ++c) {
          const double t = b.lightness_direction[c] * delta;
          r[c] = t * std::fma(t, t, std::fma(3 * r[c], t, 3 * r[c] * r[c]));
        }
        const auto correction = fast_color::multiply(b.lms_to_rgb, r);
        result = {x[0] + correction[0], x[1] + correction[1],
                  x[2] + correction[2]};
      }
      for (unsigned c = 0; c < C; ++c)
        if (copy || c >= 3)
          std::memcpy(out + c, x + c, sizeof(T));
        else
          out[c] = static_cast<T>(result[c]);
    }
  }
}
#endif

template <class T, unsigned C>
inline void Edit::span(const T* input, const double* field, T* output,
                       std::size_t count) const {
#if defined(__x86_64__)
  if (member >= 8 && native_compute::available()) {
    if (member == 8)
      perceptual_span<8, T, C>(*this, input, field, output, count);
    else
      perceptual_span<9, T, C>(*this, input, field, output, count);
    return;
  }
#endif
  std::size_t i = 0;
#if defined(__aarch64__)
  switch (member) {
#define PS_EDIT_PACKET(M)                                          \
  case M:                                                          \
    i = edit_packets<M, T, C>(*this, input, field, output, count); \
    break
    PS_EDIT_PACKET(2);
    PS_EDIT_PACKET(3);
    PS_EDIT_PACKET(4);
    PS_EDIT_PACKET(5);
    PS_EDIT_PACKET(6);
    PS_EDIT_PACKET(7);
#undef PS_EDIT_PACKET
    default:
      break;
  }
#elif defined(__x86_64__)
  if (native_compute::available()) {
    switch (member) {
#define PS_EDIT_PACKET(M)                                          \
  case M:                                                          \
    i = edit_packets<M, T, C>(*this, input, field, output, count); \
    break
      PS_EDIT_PACKET(2);
      PS_EDIT_PACKET(3);
      PS_EDIT_PACKET(4);
      PS_EDIT_PACKET(5);
      PS_EDIT_PACKET(6);
      PS_EDIT_PACKET(7);
#undef PS_EDIT_PACKET
      default:
        break;
    }
  }
#endif
  scalar<T, C>(input + C * i, field + i, output + C * i, count - i);
}
}  // namespace ps::plugin_internal::grade::feature
