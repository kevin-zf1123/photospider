// Copyright 2026 Photospider Authors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/canonical_root.hpp"
#include "07-grade/feature_curves_math.hpp"

namespace ps::plugin_internal::grade::statistics {
using namespace generation;  // NOLINT(build/namespaces)
inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](auto d) { return !d.extent; });
}
inline void cancel(const ps_kernel_invocation_v8& i) {
  if (i.cancellation.is_cancelled &&
      i.cancellation.is_cancelled(i.cancellation.context)) {
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
}
inline Region whole(const std::vector<std::uint64_t>& shape) {
  std::vector<RegionDimension> d;
  for (auto n : shape) {
    d.push_back({0, n});
  }
  return Region(std::move(d));
}
inline void typed(const OperationMetadata& in, ElementType type,
                  const std::vector<std::uint64_t>& dims) {
  const auto& t = tensor(in);
  require(t.descriptor.element_type == type && t.sample_shape() == dims,
          "grading control dtype/shape mismatch");
}
inline SchemaTemplate output_like(const OperationMetadata& in) {
  auto s = *in.result_schema;
  s.id = "photospider.tensor";
  s.version = 1;
  s.tensors[0].key = "samples";
  s.tensors[0].atomic_trailing_axes = 0;
  return s;
}
inline std::vector<OperationParameterSpec> selectors() {
  using P = OperationParameterType;
  return {{"axis", P::Int64, false},
          {"components", P::String, false},
          {"input_description", P::String, false},
          {"group", P::String, false},
          {"model", P::String, false},
          {"units", P::String, false},
          {"source_basis", P::String, false},
          {"hue_unit", P::String, false}};
}
inline fast_color::Matrix matrix(const basis_ops::Matrix& m) {
  fast_color::Matrix out;
  for (unsigned c = 0; c < 3; ++c) {
    out[c] = basis_ops::ExactRow(m, c).approximate;
  }
  return out;
}
inline void relative_units(const color_point::Selection& s) {
  const auto& i = s.interpretation;
  require(!s.semantic || i.reference == "scene" || i.reference == "display" ||
              i.reference == "scene_relative" ||
              i.reference == "display_relative",
          "grading requires relative reference");
  for (const auto& u : s.units) {
    require(u.empty() || u == "1" || u == "relative", "relative grading units");
  }
  require(!i.coordinates || i.coordinates->scale.empty() ||
              i.coordinates->scale == "relative",
          "relative grading scale");
}
inline void rgb(feature::Edit* edit, const color_point::Selection& s,
                const Parameters& p) {
  const auto& i = s.interpretation;
  require(!s.semantic || (i.model == "rgb" && i.transfer == "linear" &&
                          (i.reference == "scene" || i.reference == "display" ||
                           i.reference == "scene_relative" ||
                           i.reference == "display_relative")),
          "grading requires relative linear RGB");
  relative_units(s);
  format::RgbBasis assertion;
  assertion.preset = i.primaries;
  assertion.primaries_xy = i.primaries_xy;
  assertion.white = i.white;
  const auto b = p.count("source_basis")
                     ? basis_ops::parse_basis(text(p, "source_basis"))
                     : basis_ops::resolve(assertion);
  require((i.primaries.empty() || basis_ops::preset(i.primaries).xy == b.xy) &&
              (!i.primaries_xy || *i.primaries_xy == b.xy) &&
              (!i.white || *i.white == b.white),
          "RGB basis assertions disagree");
  auto& v = edit->brightness;
  const auto m = basis_ops::rgb_matrix(b.xy, b.white);
  v.forward = matrix(m);
  v.inverse = matrix(basis_ops::inverse(m));
  const auto a = basis_ops::adaptation(b.white, {.3127, .3290}, 1);
  v.adaptation = matrix(a);
  v.unadaptation = matrix(basis_ops::inverse(a));
  v.white = {b.white[0] / b.white[1], 1,
             (1 - b.white[0] - b.white[1]) / b.white[1]};
  v.prepare();
}
inline color_point::Selection select(const OperationMetadata& in,
                                     const Parameters& p, bool linear,
                                     bool polar = false,
                                     bool only_oklab = false) {
  try {
    auto s = color_point::select(
        in, p,
        linear  ? std::vector<std::string>{"red", "green", "blue"}
        : polar ? std::vector<std::string>{"l", "c", "h"}
                : std::vector<std::string>{"l", "a", "b"});
    require(s.axis >= 0 && s.channels.size() == 3,
            "grading selects three coordinates");
    if (!linear) {
      const auto m =
          text(p, "model", s.semantic ? s.interpretation.model.c_str() : "");
      require((polar ? m == "cielch" || m == "oklch"
                     : m == "oklab" || (!only_oklab && m == "cielab")) &&
                  (!s.semantic || s.interpretation.model == m),
              "native grading model");
      for (unsigned c = 0; c < 3; ++c) {
        const auto& u = s.units[c];
        const bool cie = (m == "cielab" || m == "cielch") &&
                         ((c == 1 && u == (polar ? "C*" : "a*")) ||
                          (!polar && c == 2 && u == "b*"));
        if (polar && c == 2) {
          require(text(p, "hue_unit", u.c_str()) == "pi_multiple" &&
                      (u.empty() || u == "pi_multiple"),
                  "selection requires pi_multiple hue");
        } else {
          require(u.empty() || u == "1" || u == "relative" || cie,
                  "native grading coordinate units");
        }
      }
    }
    return s;
  } catch (const basis_ops::CheckedFailure& f) {
    throw f.status;
  }
}

// A byte-safe affine span. All tile membership is resolved once per span,
// including negative strides and byte-unaligned imported storage.
struct Span {
  const unsigned char* data = nullptr;
  std::ptrdiff_t stride = 0;
  ElementType type = ElementType::Float64;
  std::uint64_t raw(std::uint64_t j) const {
    std::uint64_t v = 0;
    std::memcpy(&v, data + static_cast<std::ptrdiff_t>(j) * stride,
                width(type));
    return v;
  }
  double value(std::uint64_t j) const {
    const auto b = raw(j);
    if (type == ElementType::Float32) {
      float v;
      auto u = static_cast<std::uint32_t>(b);
      std::memcpy(&v, &u, 4);
      return v;
    }
    if (type == ElementType::UInt8) {
      return static_cast<std::uint8_t>(b);
    }
    if (type == ElementType::Int64) {
      std::int64_t v;
      std::memcpy(&v, &b, 8);
      return static_cast<double>(v);
    }
    return number(b);
  }
};
inline Span span(const mask::Reader& r, unsigned port, const Coordinate& at,
                 unsigned run, std::uint64_t* n) {
  const auto& v = r.view(port, 0, at);
  *n = std::min(*n, v.layout->origin[run] + v.layout->shape[run] - at[run]);
  return {static_cast<const unsigned char*>(v.allocation) +
              offset(*v.layout, v.byte_offset, at),
          v.layout->strides[run],
          static_cast<ElementType>(v.layout->element_type)};
}
// Prove the entire bounded mask before even looking up a source view.
inline bool dense_binary_selection(const Span& selection, std::uint64_t n) {
  if (selection.type != ElementType::UInt8 || selection.stride != 1 ||
      !selection.data) {
    return false;
  }
  std::uint64_t j = 0;
  for (; j + 8 <= n; j += 8) {
    std::uint64_t word;
    std::memcpy(&word, selection.data + j, sizeof(word));
    if (word != UINT64_C(0x0101010101010101)) {
      return false;
    }
  }
  for (; j < n; ++j) {
    if (selection.data[j] != 1) {
      return false;
    }
  }
  return true;
}
inline void save(const ps_kernel_mutable_view_v8& v, const Coordinate& at,
                 unsigned run, std::uint64_t j, std::uint64_t bits) {
  std::memcpy(static_cast<unsigned char*>(v.allocation) +
                  offset(*v.layout, v.byte_offset, at) +
                  static_cast<std::ptrdiff_t>(j) * v.layout->strides[run],
              &bits, width(static_cast<ElementType>(v.layout->element_type)));
}
inline double smooth(double x) {
  const double t = std::clamp(x, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
inline double plateau(double x, const double* p) {
  return smooth((x - p[0]) / (p[1] - p[0])) *
         (1.0 - smooth((x - p[2]) / (p[3] - p[2])));
}
inline double bounded_levels(double x, const double* p) {
  if (p[0] == 0 && p[1] == 1 && p[2] == 1 && p[3] == 0 && p[4] == 1 && x >= 0 &&
      x <= 1) {
    return x;
  }
  const double t = std::clamp((x - p[0]) / (p[1] - p[0]), 0.0, 1.0);
  const double u = p[2] == 1 ? t : std::pow(t, 1.0 / p[2]);
  return p[3] + (p[4] - p[3]) * u;
}
// Dynamic controls are prepared per invocation/span, never retained across
// Runs. Width division is kept exact, including tiny/overflow widths. Hoisting
// the invariant tests avoids changing source==target and signed-zero copy
// decisions.
struct PreparedLevels {
  double low, width, power, offset, range;
  bool identity, linear;
  explicit PreparedLevels(const double* p)
      : low(p[0]),
        width(p[1] - p[0]),
        power(1.0 / p[2]),
        offset(p[3]),
        range(p[4] - p[3]),
        identity(p[0] == 0 && p[1] == 1 && p[2] == 1 && p[3] == 0 && p[4] == 1),
        linear(p[2] == 1) {}
  double operator()(double x) const {
    if (identity && x >= 0 && x <= 1) {
      return x;
    }
    const double t = std::clamp((x - low) / width, 0.0, 1.0);
    return offset + range * (linear ? t : std::pow(t, power));
  }
};
inline __attribute__((always_inline)) double canonical_cie_f(double x) {
  return x > fast_color::kEpsilon
             ? canonical_root(x)
             : x / (3 * fast_color::kDelta * fast_color::kDelta) + 4.0 / 29;
}
inline __attribute__((always_inline)) fast_color::Triple canonical_lab(
    const fast_color::Triple& xyz, const fast_color::Triple& white) {
  fast_color::Triple values{xyz[0] / white[0], xyz[1], xyz[2] / white[2]};
#if defined(__x86_64__)
  auto roots = values;
  for (auto& x : roots) {
    if (!(x > fast_color::kEpsilon)) {
      x = 1.0;
    }
  }
  roots = canonical_roots(roots);
  for (unsigned c = 0; c < 3; ++c) {
    values[c] = values[c] > fast_color::kEpsilon ? roots[c]
                                                 : canonical_cie_f(values[c]);
  }
#else
  for (auto& x : values) {
    x = canonical_cie_f(x);
  }
#endif
  return {1.16 * values[1] - 0.16, 500 * (values[0] - values[1]),
          200 * (values[1] - values[2])};
}
inline __attribute__((always_inline)) fast_color::Triple exact_roots_impl(
    const feature::Edit& edit, const fast_color::Triple& x) {
  return canonical_roots(fast_color::multiply(edit.brightness.rgb_to_lms, x));
}
inline __attribute__((always_inline)) double feature_value_impl(
    const feature::Edit& e, const fast_color::Triple& x) {
  const auto& b = e.brightness;
  if (e.member <= 7) {
    return fast_color::dot(b.forward[1], x);
  }
  if (e.member == 8) {
    const double y = fast_color::dot(b.forward[1], x);
    return 1.16 * canonical_cie_f(y) - 0.16;
  }
  return fast_color::dot(fast_color::kLmsToOklab[0], exact_roots_impl(e, x));
}
#if defined(__x86_64__)
inline bool native_fma_available() {
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
}
__attribute__((target("avx2,fma"))) inline double feature_value_fma(
    const feature::Edit& e, const fast_color::Triple& x) {
  return feature_value_impl(e, x);
}
#endif
inline double feature_value(const feature::Edit& e,
                            const fast_color::Triple& x) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    return feature_value_fma(e, x);
  }
#endif
  return feature_value_impl(e, x);
}
#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) inline fast_color::Triple exact_roots_fma(
    const feature::Edit& edit, const fast_color::Triple& x) {
  return exact_roots_impl(edit, x);
}
__attribute__((target("avx2,fma"))) inline double roots_lightness_fma(
    const fast_color::Triple& roots) {
  return fast_color::dot(fast_color::kLmsToOklab[0], roots);
}
#endif
inline fast_color::Triple exact_roots(const feature::Edit& edit,
                                      const fast_color::Triple& x) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    return exact_roots_fma(edit, x);
  }
#endif
  return exact_roots_impl(edit, x);
}
inline double roots_lightness(const fast_color::Triple& roots) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    return roots_lightness_fma(roots);
  }
#endif
  return fast_color::dot(fast_color::kLmsToOklab[0], roots);
}
inline __attribute__((always_inline)) fast_color::Triple exact_lab_impl(
    const feature::Edit& edit, const fast_color::Triple& x) {
  return canonical_lab(fast_color::multiply(edit.brightness.forward, x),
                       edit.brightness.white);
}
#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) inline fast_color::Triple exact_lab_fma(
    const feature::Edit& edit, const fast_color::Triple& x) {
  return exact_lab_impl(edit, x);
}
#endif
inline fast_color::Triple exact_lab(const feature::Edit& edit,
                                    const fast_color::Triple& x) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    return exact_lab_fma(edit, x);
  }
#endif
  return exact_lab_impl(edit, x);
}
template <unsigned Kind, class Read, class Emit>
inline __attribute__((always_inline)) void measured_rgb_reduce_impl(
    const feature::Edit& edit, std::uint64_t n, const Read& read,
    const Emit& emit) {
  for (std::uint64_t j = 0; j < n; ++j) {
    fast_color::Triple rgb, coordinates{};
    bool needed = false;
    if (!read(j, &rgb, &needed)) {
      continue;
    }
    double value;
    if constexpr (Kind <= 2) {
      value = fast_color::dot(edit.brightness.forward[1], rgb);
    } else if constexpr (Kind == 3) {
      if (needed) {
        coordinates = exact_lab_impl(edit, rgb);
        value = coordinates[0];
      } else {
        value = 1.16 * canonical_cie_f(
                           fast_color::dot(edit.brightness.forward[1], rgb)) -
                .16;
      }
    } else {
      coordinates = exact_roots_impl(edit, rgb);
      value = fast_color::dot(fast_color::kLmsToOklab[0], coordinates);
    }
    emit(j, value, coordinates);
  }
}
template <class Read, class Emit>
inline __attribute__((always_inline)) void measured_rgb_reduce_impl(
    unsigned kind, const feature::Edit& edit, std::uint64_t n, const Read& read,
    const Emit& emit) {
  if (kind <= 2) {
    measured_rgb_reduce_impl<0>(edit, n, read, emit);
  } else if (kind == 3) {
    measured_rgb_reduce_impl<3>(edit, n, read, emit);
  } else {
    measured_rgb_reduce_impl<4>(edit, n, read, emit);
  }
}
#if defined(__x86_64__)
template <class Read, class Emit>
__attribute__((target("avx2,fma"))) inline void measured_rgb_reduce_fma(
    unsigned kind, const feature::Edit& edit, std::uint64_t n, const Read& read,
    const Emit& emit) {
  measured_rgb_reduce_impl(kind, edit, n, read, emit);
}
#endif
// A bounded scan amortizes ISA dispatch without staging dynamic pixel arrays.
// Read rejects zero effective mass before source access; Emit consumes each
// canonical feature immediately in row-major order. libm/FMA rules are intact.
template <class Read, class Emit>
inline void measured_rgb_reduce(unsigned kind, const feature::Edit& edit,
                                std::uint64_t n, const Read& read,
                                const Emit& emit) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    measured_rgb_reduce_fma(kind, edit, n, read, emit);
    return;
  }
#endif
  measured_rgb_reduce_impl(kind, edit, n, read, emit);
}
/** Invocation-owned dynamic exposure descriptor. State one short-circuits EV
 * access and EV zero short-circuits exp2, preserving identity and AOV copies.
 * Refresh from this invocation's controls; no prepared frame content survives.
 */
struct PreparedExposure {
  bool copy = true;
  double gain = 1.0;
  void prepare(const double* controls) {
    copy = controls[5] != 0 || controls[0] == 0.0;
    gain = copy ? 1.0 : std::exp2(controls[0]);
  }
};
inline double exposure_value(double x, double gain) {
  return x * gain;
}
inline fast_color::Triple automatic_rgb(const feature::Edit& edit,
                                        const fast_color::Triple& x,
                                        const double* controls, bool* copy);
/** Native per-component automatic application, shared by affine and packed
 * callers. The returned copy flag retains the original sample representation.
 */
inline double automatic_component(const feature::Edit& edit, unsigned kind,
                                  const fast_color::Triple& rgb,
                                  unsigned component, double x,
                                  const double* controls, bool* copy) {
  *copy = controls[5] != 0 || (kind == 0 && controls[0] == 0);
  if (*copy) {
    return x;
  }
  if (kind == 0) {
    return x * std::exp2(controls[0]);
  }
  if (kind == 5) {
    return bounded_levels(x, controls);
  }
  if (kind == 3 || kind == 4) {
    return automatic_rgb(edit, rgb, controls, copy)[component];
  }
  const double source = feature_value(edit, rgb);
  return edit.replace(rgb, bounded_levels(source, controls), copy)[component];
}
inline __attribute__((always_inline)) fast_color::Triple
automatic_rgb_from_roots(const feature::Edit& edit, const fast_color::Triple& x,
                         const fast_color::Triple& roots,
                         const double* controls, bool* copy) {
  const double source = fast_color::dot(fast_color::kLmsToOklab[0], roots);
  const double target = bounded_levels(source, controls);
  *copy = source == target;
  if (*copy) {
    return x;
  }
  const double delta = target - source;
  fast_color::Triple cubes;
  for (unsigned c = 0; c < 3; ++c) {
    const double r = roots[c],
                 t = edit.brightness.lightness_direction[c] * delta;
    cubes[c] = t * std::fma(t, t, std::fma(3.0 * r, t, 3.0 * r * r));
  }
  const auto correction =
      fast_color::multiply(edit.brightness.lms_to_rgb, cubes);
  return {x[0] + correction[0], x[1] + correction[1], x[2] + correction[2]};
}
/** Complete RGB levels in one native feature/reconstruction pass. */
inline __attribute__((always_inline)) fast_color::Triple automatic_rgb(
    const feature::Edit& edit, const fast_color::Triple& x,
    const double* controls, bool* copy) {
  const auto& b = edit.brightness;
  if (edit.member == 5 || edit.member == 7) {
    const double source = fast_color::dot(b.forward[1], x);
    const double target = bounded_levels(source, controls);
    *copy = source == target;
    if (*copy) {
      return x;
    }
    if (edit.member == 5) {
      if (source == 0) {
        return {target, target, target};
      }
      const double gain = target / source;
      return {x[0] * gain, x[1] * gain, x[2] * gain};
    }
    const double delta = target - source;
    return {std::fma(b.inverse[0][1], delta, x[0]),
            std::fma(b.inverse[1][1], delta, x[1]),
            std::fma(b.inverse[2][1], delta, x[2])};
  }
  if (edit.member == 8) {
    auto lab = canonical_lab(fast_color::multiply(b.forward, x), b.white);
    const double target = bounded_levels(lab[0], controls);
    *copy = lab[0] == target;
    if (*copy) {
      return x;
    }
    lab[0] = target;
    return fast_color::multiply(b.inverse,
                                fast_color::lab_to_xyz(lab, b.white));
  }
  const auto roots = canonical_roots(fast_color::multiply(b.rgb_to_lms, x));
  const double source = fast_color::dot(fast_color::kLmsToOklab[0], roots);
  const double target = bounded_levels(source, controls);
  *copy = target == source;
  if (*copy) {
    return x;
  }
  const double delta = target - source;
  fast_color::Triple cubes;
  for (unsigned c = 0; c < 3; ++c) {
    const double r = roots[c], t = b.lightness_direction[c] * delta;
    cubes[c] = t * std::fma(t, t, std::fma(3.0 * r, t, 3.0 * r * r));
  }
  const auto correction = fast_color::multiply(b.lms_to_rgb, cubes);
  return {x[0] + correction[0], x[1] + correction[1], x[2] + correction[2]};
}
#if defined(__aarch64__) || defined(__x86_64__)
#if defined(__x86_64__)
#define PS_STAT_PACKET_TARGET __attribute__((target("avx2,fma")))
#else
#define PS_STAT_PACKET_TARGET
#endif
PS_STAT_PACKET_TARGET inline affine::Packet packet_dot(
    const fast_color::Triple& row, const affine::Packet* x) {
  namespace v = affine;
  return v::madd(
      v::splat(row[2]), x[2],
      v::madd(v::splat(row[1]), x[1], v::mul(v::splat(row[0]), x[0])));
}
template <class T, unsigned C>
PS_STAT_PACKET_TARGET inline void dense_linear_span_impl(
    const T* input, double* output, std::uint64_t n,
    const fast_color::Triple& row, bool* negative_zero) {
  namespace v = affine;
  std::uint64_t j = 0;
  bool negative = false;
  for (; j + v::kLanes <= n; j += v::kLanes) {
    T components[3][v::kLanes];
    for (unsigned c = 0; c < 3; ++c) {
      for (unsigned lane = 0; lane < v::kLanes; ++lane) {
        std::memcpy(&components[c][lane], input + C * (j + lane) + c,
                    sizeof(T));
      }
    }
    const v::Packet rgb[]{v::load(components[0]), v::load(components[1]),
                          v::load(components[2])};
    const auto value = packet_dot(row, rgb);
    v::save(output + j, value);
#if defined(__x86_64__)
    negative |=
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(
            _mm256_castpd_si256(value), _mm256_set1_epi64x(INT64_MIN)))) != 0;
#else
    const auto zeros =
        vceqq_u64(vreinterpretq_u64_f64(value), vdupq_n_u64(UINT64_C(1) << 63));
    negative |= (vgetq_lane_u64(zeros, 0) | vgetq_lane_u64(zeros, 1)) != 0;
#endif
  }
  for (; j < n; ++j) {
    const auto* pixel = input + C * j;
    const double value = fast_color::dot(row, {pixel[0], pixel[1], pixel[2]});
    output[j] = value;
    negative |= value == 0.0 && std::signbit(value);
  }
  *negative_zero = negative;
}
PS_STAT_PACKET_TARGET inline affine::Packet packet_levels(
    const PreparedLevels& p, affine::Packet x) {
  namespace v = affine;
  const auto zero = v::splat(0.0), one = v::splat(1.0);
  auto t = feature::divide(v::sub(x, v::splat(p.low)), v::splat(p.width));
  t = v::choose(feature::greater(zero, t), zero, t);
  t = v::choose(feature::greater(t, one), one, t);
  const auto value = v::add(v::splat(p.offset), v::mul(v::splat(p.range), t));
  return p.identity
             ? v::choose(v::both(v::ge(x, zero), v::le(x, one)), x, value)
             : value;
}
PS_STAT_PACKET_TARGET inline affine::Packet packet_cie_g(affine::Packet x) {
  namespace v = affine;
  return v::choose(feature::greater(x, v::splat(fast_color::kDelta)),
                   v::mul(v::mul(x, x), x),
                   v::mul(v::splat(3 * fast_color::kDelta * fast_color::kDelta),
                          v::sub(x, v::splat(4.0 / 29))));
}
// Each packet snapshots every original component before any output store.
// This also separates interleaved source/coordinate loads from stores at
// unfavourable partial-address offsets. No image-sized workspace is used.
template <unsigned Kind, class T, unsigned C>
PS_STAT_PACKET_TARGET inline std::uint64_t automatic_packets(
    const feature::Edit& edit, const T* input, const double* coordinates,
    T* output, std::uint64_t n, const double* controls,
    double exposure_gain = 1.0) {
  namespace v = affine;
  const double identity_controls[6]{0, 1, 1, 0, 1, 0};
  PreparedLevels levels(Kind == 0 || Kind == 5 ? identity_controls : controls);
  if constexpr (Kind != 0) {
    if (!levels.linear) {
      return 0;
    }
  }
  PreparedLevels per_channel[3]{PreparedLevels(identity_controls),
                                PreparedLevels(identity_controls),
                                PreparedLevels(identity_controls)};
  if constexpr (Kind == 5) {
    for (unsigned c = 0; c < 3; ++c) {
      if (controls[6 * c + 5] == 0) {
        per_channel[c] = PreparedLevels(controls + 6 * c);
        if (!per_channel[c].linear) {
          return 0;
        }
      }
    }
  }
  const auto zero = v::splat(0.0), one = v::splat(1.0);
  const auto gain = v::splat(exposure_gain);
  std::uint64_t j = 0;
  for (; j + v::kLanes <= n; j += v::kLanes) {
    T source[C][v::kLanes], target[C][v::kLanes];
    for (unsigned c = 0; c < C; ++c) {
      for (unsigned lane = 0; lane < v::kLanes; ++lane) {
        std::memcpy(&source[c][lane], input + C * (j + lane) + c, sizeof(T));
      }
    }
    v::Packet x[3]{v::load(source[0]), v::load(source[1]), v::load(source[2])};
    v::Packet result[3]{x[0], x[1], x[2]};
    v::Mask copy[3]{feature::equal(zero, one), feature::equal(zero, one),
                    feature::equal(zero, one)};
    if constexpr (Kind == 0) {
      for (unsigned c = 0; c < 3; ++c) {
        result[c] = v::mul(x[c], gain);
      }
    } else if constexpr (Kind == 5) {
      for (unsigned c = 0; c < 3; ++c) {
        result[c] = packet_levels(per_channel[c], x[c]);
        copy[c] = controls[6 * c + 5] != 0 ? feature::equal(zero, zero)
                                           : feature::equal(zero, one);
      }
    } else {
      v::Packet original[3];
      if constexpr (Kind == 3 || Kind == 4) {
        double a[3][v::kLanes];
        for (unsigned c = 0; c < 3; ++c) {
          for (unsigned lane = 0; lane < v::kLanes; ++lane) {
            a[c][lane] = coordinates[3 * (j + lane) + c];
          }
          original[c] = v::load(a[c]);
        }
      }
      const auto source_value =
          Kind == 3   ? original[0]
          : Kind == 4 ? packet_dot(fast_color::kLmsToOklab[0], original)
                      : packet_dot(edit.brightness.forward[1], x);
      const auto target_value = packet_levels(levels, source_value);
      copy[0] = copy[1] = copy[2] = feature::equal(source_value, target_value);
      const auto delta = v::sub(target_value, source_value);
      if constexpr (Kind == 1) {
        const auto black = feature::equal(source_value, zero);
        const auto ratio =
            feature::divide(target_value, v::choose(black, one, source_value));
        for (unsigned c = 0; c < 3; ++c) {
          result[c] = v::choose(black, target_value, v::mul(x[c], ratio));
        }
      } else if constexpr (Kind == 2) {
        for (unsigned c = 0; c < 3; ++c) {
          result[c] =
              v::madd(v::splat(edit.brightness.inverse[c][1]), delta, x[c]);
        }
      } else if constexpr (Kind == 3) {
        const auto b = feature::divide(v::add(target_value, v::splat(.16)),
                                       v::splat(1.16));
        v::Packet xyz[3]{
            v::mul(v::splat(edit.brightness.white[0]),
                   packet_cie_g(
                       v::add(b, feature::divide(original[1], v::splat(500))))),
            packet_cie_g(b),
            v::mul(v::splat(edit.brightness.white[2]),
                   packet_cie_g(v::sub(
                       b, feature::divide(original[2], v::splat(200)))))};
        for (unsigned c = 0; c < 3; ++c) {
          result[c] = packet_dot(edit.brightness.inverse[c], xyz);
        }
      } else {
        v::Packet cubes[3];
        for (unsigned c = 0; c < 3; ++c) {
          const auto r = original[c],
                     t = v::mul(
                         v::splat(edit.brightness.lightness_direction[c]),
                         delta);
          const auto three_r = v::mul(v::splat(3.0), r);
          cubes[c] =
              v::mul(t, v::madd(t, t, v::madd(three_r, t, v::mul(three_r, r))));
        }
        for (unsigned c = 0; c < 3; ++c) {
          result[c] =
              v::add(x[c], packet_dot(edit.brightness.lms_to_rgb[c], cubes));
        }
      }
    }
    for (unsigned c = 0; c < C; ++c) {
      if (c >= 3) {
        std::memcpy(target[c], source[c], sizeof(source[c]));
      } else {
        v::save_preserving(target[c], source[c], result[c], copy[c]);
      }
    }
    for (unsigned lane = 0; lane < v::kLanes; ++lane) {
      for (unsigned c = 0; c < C; ++c) {
        std::memcpy(output + C * (j + lane) + c, &target[c][lane], sizeof(T));
      }
    }
  }
  return j;
}
#undef PS_STAT_PACKET_TARGET
#endif
// The caller proves dense complete RGB and all-one selection. Refusal writes
// nothing; accepted spans retain the canonical dot and row-major deposit order.
template <class T, unsigned C>
inline bool dense_linear_span(const T* input, double* output, std::uint64_t n,
                              const fast_color::Triple& row,
                              bool* negative_zero) {
#if defined(__x86_64__)
  if (!native_fma_available()) {
    return false;
  }
#endif
#if defined(__aarch64__) || defined(__x86_64__)
  dense_linear_span_impl<T, C>(input, output, n, row, negative_zero);
  return true;
#else
  return false;
#endif
}
/** Packed application consumes only this Run's explicit controls. The same
 * kernel is used by ordinary operator invocations and compute measurements.
 * Source and destination must be aligned disjoint packed C=3/4 spans.
 */
template <unsigned Kind, class T, unsigned C>
inline __attribute__((always_inline)) void automatic_span_impl(
    const feature::Edit& edit, const T* input, T* output, std::uint64_t n,
    const double* controls, const PreparedExposure* prepared = nullptr) {
  PreparedExposure exposure;
  if constexpr (Kind == 0) {
    if (prepared) {
      exposure = *prepared;
    } else {
      exposure.prepare(controls);
    }
    if (exposure.copy) {
      std::memcpy(output, input, n * C * sizeof(T));
      return;
    }
  } else if constexpr (Kind != 5) {
    if (controls[5] != 0) {
      std::memcpy(output, input, n * C * sizeof(T));
      return;
    }
  }
  const double gain = exposure.gain;
  std::uint64_t first = 0;
#if defined(__aarch64__) || defined(__x86_64__)
  if constexpr (Kind == 0 || Kind == 1 || Kind == 2 || Kind == 5) {
#if defined(__x86_64__)
    if (native_fma_available())
#endif
      first = automatic_packets<Kind, T, C>(edit, input, nullptr, output, n,
                                            controls, gain);
  }
#endif
  const double identity_controls[6]{0, 1, 1, 0, 1, 0};
  const PreparedLevels levels(Kind == 1 || Kind == 2 ? controls
                                                     : identity_controls);
  for (std::uint64_t i = first; i < n; ++i) {
    const auto* x = input + C * i;
    auto* y = output + C * i;
    if constexpr (Kind == 0) {
      for (unsigned c = 0; c < 3; ++c) {
        y[c] = static_cast<T>(exposure_value(x[c], gain));
      }
    } else if constexpr (Kind == 5) {
      for (unsigned c = 0; c < 3; ++c) {
        const auto* p = controls + 6 * c;
        if (p[5] != 0) {
          std::memcpy(y + c, x + c, sizeof(T));
        } else {
          y[c] = static_cast<T>(bounded_levels(x[c], p));
        }
      }
    } else {
      bool copy;
      fast_color::Triple v;
      if constexpr (Kind == 1 || Kind == 2) {
        const double source =
            fast_color::dot(edit.brightness.forward[1], {x[0], x[1], x[2]});
        const double target = levels(source);
        copy = source == target;
        if constexpr (Kind == 1) {
          const double ratio = source == 0 ? 1 : target / source;
          for (unsigned c = 0; c < 3; ++c) {
            v[c] = source == 0 ? target : x[c] * ratio;
          }
        } else {
          for (unsigned c = 0; c < 3; ++c) {
            v[c] =
                std::fma(edit.brightness.inverse[c][1], target - source, x[c]);
          }
        }
      } else {
        v = automatic_rgb(edit, {x[0], x[1], x[2]}, controls, &copy);
      }
      if (copy) {
        std::memcpy(y, x, 3 * sizeof(T));
      } else {
        for (unsigned c = 0; c < 3; ++c) {
          y[c] = static_cast<T>(v[c]);
        }
      }
    }
    if constexpr (C == 4) {
      std::memcpy(y + 3, x + 3, sizeof(T));
    }
  }
}
#if defined(__x86_64__)
template <unsigned Kind, class T, unsigned C>
__attribute__((target("avx2,fma"))) inline void automatic_span_fma(
    const feature::Edit& edit, const T* input, T* output, std::uint64_t n,
    const double* controls, const PreparedExposure* prepared) {
  automatic_span_impl<Kind, T, C>(edit, input, output, n, controls, prepared);
}
#endif
template <unsigned Kind, class T, unsigned C>
inline void automatic_span(const feature::Edit& edit, const T* input, T* output,
                           std::uint64_t n, const double* controls,
                           const PreparedExposure* prepared = nullptr) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    automatic_span_fma<Kind, T, C>(edit, input, output, n, controls, prepared);
    return;
  }
#endif
  automatic_span_impl<Kind, T, C>(edit, input, output, n, controls, prepared);
}
template <unsigned Kind>
inline __attribute__((always_inline)) fast_color::Triple
automatic_rgb_from_coordinates(const feature::Edit& edit,
                               const fast_color::Triple& x,
                               fast_color::Triple coordinates,
                               const double* controls, bool* copy) {
  if constexpr (Kind == 3) {
    const double target = bounded_levels(coordinates[0], controls);
    *copy = target == coordinates[0];
    if (*copy) {
      return x;
    }
    coordinates[0] = target;
    return fast_color::multiply(
        edit.brightness.inverse,
        fast_color::lab_to_xyz(coordinates, edit.brightness.white));
  } else {
    return automatic_rgb_from_roots(edit, x, coordinates, controls, copy);
  }
}
template <unsigned Kind, class T, unsigned C>
inline __attribute__((always_inline)) void cached_automatic_span_impl(
    const feature::Edit& edit, const T* input, const double* roots, T* output,
    std::uint64_t n, const double* controls) {
  if (controls[5] != 0) {
    std::memcpy(output, input, n * C * sizeof(T));
    return;
  }
  std::uint64_t first = 0;
#if defined(__aarch64__) || defined(__x86_64__)
#if defined(__x86_64__)
  if (native_fma_available())
#endif
    first =
        automatic_packets<Kind, T, C>(edit, input, roots, output, n, controls);
#endif
  for (std::uint64_t j = first; j < n; ++j) {
    const auto* x = input + C * j;
    auto* y = output + C * j;
    bool copy;
    const auto v = automatic_rgb_from_coordinates<Kind>(
        edit, {x[0], x[1], x[2]},
        {roots[3 * j], roots[3 * j + 1], roots[3 * j + 2]}, controls, &copy);
    if (copy) {
      std::memcpy(y, x, 3 * sizeof(T));
    } else {
      for (unsigned c = 0; c < 3; ++c) {
        y[c] = static_cast<T>(v[c]);
      }
    }
    if constexpr (C == 4) {
      std::memcpy(y + 3, x + 3, sizeof(T));
    }
  }
}
#if defined(__x86_64__)
template <unsigned Kind, class T, unsigned C>
__attribute__((target("avx2,fma"))) inline void cached_automatic_span_fma(
    const feature::Edit& edit, const T* input, const double* roots, T* output,
    std::uint64_t n, const double* controls) {
  cached_automatic_span_impl<Kind, T, C>(edit, input, roots, output, n,
                                         controls);
}
#endif
template <unsigned Kind, class T, unsigned C>
inline void cached_automatic_span(const feature::Edit& edit, const T* input,
                                  const double* roots, T* output,
                                  std::uint64_t n, const double* controls) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    cached_automatic_span_fma<Kind, T, C>(edit, input, roots, output, n,
                                          controls);
    return;
  }
#endif
  cached_automatic_span_impl<Kind, T, C>(edit, input, roots, output, n,
                                         controls);
}
#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) inline fast_color::Triple
cached_automatic_pixel_fma(const feature::Edit& edit,
                           const fast_color::Triple& x,
                           const fast_color::Triple& roots,
                           const double* controls, bool* copy, unsigned kind) {
  return kind == 3
             ? automatic_rgb_from_coordinates<3>(edit, x, roots, controls, copy)
             : automatic_rgb_from_coordinates<4>(edit, x, roots, controls,
                                                 copy);
}
#endif
inline fast_color::Triple cached_automatic_pixel(
    const feature::Edit& edit, const fast_color::Triple& x,
    const fast_color::Triple& roots, const double* controls, bool* copy,
    unsigned kind = 4) {
#if defined(__x86_64__)
  if (native_fma_available()) {
    return cached_automatic_pixel_fma(edit, x, roots, controls, copy, kind);
  }
#endif
  return kind == 3
             ? automatic_rgb_from_coordinates<3>(edit, x, roots, controls, copy)
             : automatic_rgb_from_coordinates<4>(edit, x, roots, controls,
                                                 copy);
}
struct Metering {
  std::uint64_t h, w, x0 = 0, y0 = 0, x1, y1;
  bool weighted;
  Metering(std::uint64_t height, std::uint64_t width, const Parameters& p)
      : h(height), w(width), x1(width), y1(height) {
    const auto m = text(p, "selection_mode", "binary");
    require(m == "binary" || m == "weighted", "selection_mode");
    weighted = m == "weighted";
    // The ABI static parameter vocabulary has no array alternative.
    x0 = integer(p, "roi_x0", 0);
    y0 = integer(p, "roi_y0", 0);
    x1 = integer(p, "roi_x1", w);
    y1 = integer(p, "roi_y1", h);
  }
  Region region() const { return Region({{y0, y1 - y0}, {x0, x1 - x0}}); }
  void selection(const OperationMetadata& in) const {
    typed(in, weighted ? ElementType::Float64 : ElementType::UInt8, {h, w});
  }
};
inline std::vector<OperationParameterSpec> metering_parameters(
    bool roi = true) {
  using P = OperationParameterType;
  std::vector<OperationParameterSpec> p{{"selection_mode", P::String, false}};
  if (roi) {
    for (const auto* key : {"roi_x0", "roi_y0", "roi_x1", "roi_y1"}) {
      p.push_back({key, P::Int64, false});
    }
  }
  return p;
}
}  // namespace ps::plugin_internal::grade::statistics
