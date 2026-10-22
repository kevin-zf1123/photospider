#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_ratio.hpp"
#include "04-mask-morphology/common.hpp"
#include "04-mask-morphology/fast.hpp"
#include "common/registry.hpp"
#include "photospider/ops/tensor_description.hpp"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace ps::plugin_internal::mask {
namespace {
enum class Kind {
  Binary,
  Fuzzy,
  Independent,
  Invert,
  Threshold,
  Range,
  Soft,
  SoftRange,
  Nonzero,
  Affect,
  Multiply,
  Alpha
};
using Ratio = numeric_ops::RatioWorkspace;
using Compact = model_ops::RationalMathStorage<32>;
struct SoftScratch final {
  Exact wide{numeric_ops::SequenceProfile::Strict};
  Compact small{numeric_ops::SequenceProfile::Strict};
  explicit SoftScratch(const Work& work) {
    wide.bind(work);
    small.bind(work);
  }
};
bool compact_value(double v) {
  return v == 0 || (std::abs(v) >= 0x1p-100 && std::abs(v) <= 0x1p100);
}
struct Pointwise final : Program {
  Kind kind;
  ElementType type;
  unsigned mode = 0;
  double lo = 0, hi = 0, wl = 0, wu = 0;
  bool closed_lo = true, closed_hi = true, smooth = false, compact = false;
  std::vector<std::uint64_t> selected;
  Pointwise(Kind k, const std::vector<OperationMetadata>& in,
            const Parameters& p)
      : kind(k) {
    const bool planar =
        k == Kind::Affect || k == Kind::Multiply || k == Kind::Alpha;
    const auto& s = image(in[0], planar ? 3 : 2, k == Kind::Nonzero);
    type = k == Kind::Nonzero ? output_dtype(p) : s.descriptor.element_type;
    if (k == Kind::Binary || k == Kind::Fuzzy || k == Kind::Independent) {
      same(in[0], in[1]);
      const auto name = text(p, "operation");
      const std::array<std::string, 4> names{"and", "or", "xor", "subtract"};
      const auto it = std::find(names.begin(), names.end(), name);
      parameter(it != names.end(), "unknown mask algebra operation");
      mode = static_cast<unsigned>(it - names.begin());
    }
    if (k == Kind::Threshold) {
      lo = real(p, "threshold");
      const auto name = text(p, "comparison");
      const std::array<std::string, 4> names{"ge", "gt", "le", "lt"};
      const auto it = std::find(names.begin(), names.end(), name);
      parameter(it != names.end(), "unknown mask comparator");
      mode = static_cast<unsigned>(it - names.begin());
    }
    if (k == Kind::Range || k == Kind::SoftRange) {
      lo = real(p, "lower");
      hi = real(p, "upper");
      if (k == Kind::Range) {
        closed_lo = boolean(p, "lower_closed");
        closed_hi = boolean(p, "upper_closed");
      } else {
        wl = real(p, "lower_width");
        wu = real(p, "upper_width");
      }
    }
    if (k == Kind::Soft) {
      lo = real(p, "threshold");
      wl = real(p, "width");
    }
    if (k == Kind::Soft || k == Kind::SoftRange) {
      const auto name = text(p, "curve");
      parameter(name == "linear" || name == "smoothstep", "unknown mask curve");
      smooth = name == "smoothstep";
      scratch_bytes = sizeof(SoftScratch);
      compact = compact_value(lo) && compact_value(hi) && compact_value(wl) &&
                compact_value(wu);
    }
    if (k == Kind::Independent || k == Kind::Affect)
      scratch_bytes = sizeof(Ratio);
    if (planar) {
      const auto dims = s.sample_shape();
      if (k == Kind::Affect)
        same(in[0], in[1]);
      const auto port = k == Kind::Affect ? 2U : 1U;
      const auto& m = image(in[port], 2);
      require(
          m.sample_shape() == std::vector<std::uint64_t>{dims[1], dims[2]} &&
              m.descriptor.element_type == type,
          "mask spatial shape/dtype");
      if (k != Kind::Alpha) {
        selected = channels(text(p, "channels"), dims[0]);
      } else {
        const auto a = integer(p, "alpha_channel");
        require(a >= 0 && static_cast<std::uint64_t>(a) < dims[0],
                "mask alpha channel outside axis");
        selected = {static_cast<std::uint64_t>(a)};
        const auto d = take(
            tensor_description_from_parameter(text(p, "input_description")));
        check(validate_tensor_description(d, s.descriptor));
        require(d.channel_axis == std::optional<std::uint32_t>{0},
                "mask alpha requires planar description");
        bool matched = false;
        for (const auto& g : d.groups) {
          if (g.alpha == selected[0]) {
            matched = true;
            const auto association = g.interpretation.association.empty()
                                         ? d.association
                                         : g.interpretation.association;
            require(association == "straight",
                    "mask alpha requires straight image");
            validate_description(in[0], p, g.indices, false);
          }
        }
        require(matched, "alpha_channel must match internal described alpha");
      }
    }
    add_output(this, spatial_schema(type, s.sample_shape()));
  }
  bool applies(std::uint64_t c) const {
    return std::find(selected.begin(), selected.end(), c) != selected.end();
  }
  bool planar() const {
    return kind == Kind::Affect || kind == Kind::Multiply ||
           kind == Kind::Alpha;
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    if (!planar()) {
      std::vector<ReadRegion> result{{0, 0, q}};
      if (kind == Kind::Binary || kind == Kind::Fuzzy ||
          kind == Kind::Independent)
        result.push_back({1, 0, q});
      return result;
    }
    std::vector<ReadRegion> result{{0, 0, q}};
    const auto c = q.dimensions()[0];
    bool masked = false;
    for (auto i = c.offset; i < c.offset + c.extent; ++i) {
      if (!applies(i))
        continue;
      masked = true;
      if (kind == Kind::Affect)
        result.push_back({1, 0, channel_region(i, q)});
    }
    if (masked)
      result.push_back({kind == Kind::Affect ? 2U : 1U, 0, spatial_region(q)});
    return result;
  }
  std::uint64_t polynomial(Ratio& e, double a, double b, double m, bool narrow,
                           const Work& work) const {
    // Binary64 interval evaluation certifies most Float32 outputs cheaply.
    if (narrow) {
      const auto x = Fast::point(a), y = Fast::point(b), z = Fast::point(m);
      const auto f = kind == Kind::Affect ? (Fast::point(1) - z) * x + z * y
                     : mode == 1          ? x + y - x * y
                                          : x + y - Fast::point(2) * x * y;
      if (f.finite() && f.nonzero()) {
        const auto l = convert(f.low, true), u = convert(f.high, true);
        if (l == u)
          return l;
      }
    }
    e.numerator.words.fill(0);
    e.negative = false;
    const auto x = numeric_ops::BinaryParts::decode(bits(a), false);
    const auto y = numeric_ops::BinaryParts::decode(bits(b), false);
    if (kind == Kind::Affect) {
      const auto z = numeric_ops::BinaryParts::decode(bits(m), false);
      e.term.set(x, 2148);
      e.add_term(x.negative);
      e.product_term(z, y);
      e.product_term(z, x, true);
    } else {
      e.term.set(x, 2148);
      e.add_term(x.negative);
      e.term.set(y, 2148);
      e.add_term(y.negative);
      e.product_term(x, y, true);
      if (mode == 2)
        e.product_term(x, y, true);
    }
    e.denominator.words.fill(0);
    e.denominator.words[0] = 1;
    return take(e.round(narrow, work, -2148));
  }
  template <class Arithmetic>
  std::uint64_t soft_exact(Arithmetic& e, double a, bool narrow) const {
    e.reset();
    const auto x = e.binary(bits(a));
    auto r = soft_edge(e, x, lo, wl, smooth);
    if (kind == Kind::SoftRange) {
      auto upper = soft_edge(e, x, hi, wu, smooth, true);
      if (e.order(upper, r) < 0)
        r = std::move(upper);
    }
    return rounded(e, r, narrow);
  }
  std::uint64_t soft(SoftScratch& scratch, double a, bool narrow) const {
    const auto edge = [&](double t, double w, bool upper) {
      if (w == 0)
        return Fast::point(upper ? a <= t : a >= t);
      const auto q =
          (Fast::point(a) - Fast::point(t)) / Fast::point(w) + Fast::point(.5);
      const auto value = fast_curve(q, smooth);
      return upper ? unit(Fast::point(1) - value) : value;
    };
    auto enclosure = edge(lo, wl, false);
    if (kind == Kind::SoftRange) {
      const auto upper = edge(hi, wu, true);
      if (upper.finite() && enclosure.finite())
        enclosure = {std::min(enclosure.low, upper.low),
                     std::min(enclosure.high, upper.high)};
      else
        enclosure = {-INFINITY, INFINITY};
    }
    if (auto result = certified(enclosure, narrow))
      return *result;
    // On this exponent range the cubic and cross-multiplied min predicates
    // fit within 2048 bits. Extreme values retain the complete arena.
    if (compact && compact_value(a))
      return soft_exact(scratch.small, a, narrow);
    return soft_exact(scratch.wide, a, narrow);
  }
  template <class T>
  T evaluate(T a, T b, T m, SoftScratch* exact, Ratio* ratio,
             const Work& work) const {
    const bool narrow = sizeof(T) == 4;
    const auto cast = [&](std::uint64_t raw) {
      if constexpr (sizeof(T) == 4) {
        const auto u = static_cast<std::uint32_t>(raw);
        T x;
        std::memcpy(&x, &u, 4);
        return x;
      } else {
        return number(raw);
      }
    };
    T result = 0;
    switch (kind) {
      case Kind::Binary: {
        const bool x = a == 1, y = b == 1;
        return static_cast<T>(mode == 0   ? x && y
                              : mode == 1 ? x || y
                              : mode == 2 ? x != y
                                          : x && !y);
      }
      case Kind::Fuzzy:
        if (mode == 0)
          return a <= b ? a : b;
        if (mode == 1)
          return a >= b ? a : b;
        result = mode == 2 ? std::abs(a - b) : std::min(a, T(1) - b);
        return result == 0 ? T(0) : result;
      case Kind::Independent:
        if (mode == 0)
          result = a * b;
        else if (mode == 3)
          result = std::fma(-a, b, a);
        else
          return cast(polynomial(*ratio, a, b, 0, narrow, work));
        return result == 0 ? T(0) : result;
      case Kind::Invert:
        return T(1) - a;
      case Kind::Threshold:
        return static_cast<T>(mode == 0   ? a >= lo
                              : mode == 1 ? a > lo
                              : mode == 2 ? a <= lo
                                          : a < lo);
      case Kind::Range:
        return static_cast<T>((closed_lo ? a >= lo : a > lo) &&
                              (closed_hi ? a <= hi : a < hi));
      case Kind::Soft:
      case Kind::SoftRange:
        return cast(soft(*exact, a, narrow));
      case Kind::Affect:
        return cast(polynomial(*ratio, a, b, m, narrow, work));
      case Kind::Multiply:
      case Kind::Alpha:
        if (kind == Kind::Alpha && m == 1)
          return a;
        result = a * m;
        return a == 0 || m == 0 ? T(0) : result;
      case Kind::Nonzero:
        return static_cast<T>(a != 0);
    }
    return result;
  }
  template <class T>
  void rows(const Kernel& kernel, const ps_kernel_invocation_v8& invocation,
            const Work& work, SoftScratch* e, Ratio* ratio) const {
    Reader r{kernel, invocation};
    const auto& out = invocation.outputs[0];
    const auto& l = *out.layout;
    const auto cy = planar() ? 1U : 0U, xx = cy + 1;
    const auto first_c = planar() ? l.origin[0] : 0;
    const auto count_c = planar() ? l.shape[0] : 1;
    for (auto c = first_c; c < first_c + count_c; ++c) {
      const bool active = !planar() || applies(c);
      for (auto y = l.origin[cy]; y < l.origin[cy] + l.shape[cy]; ++y) {
        auto x = l.origin[xx];
        while (x < l.origin[xx] + l.shape[xx]) {
          Coordinate at = planar() ? Coordinate{c, y, x} : Coordinate{y, x};
          const auto& a = r.view(0, 0, at);
          auto n = std::min<std::uint64_t>(
              {l.origin[xx] + l.shape[xx] - x,
               a.layout->origin[xx] + a.layout->shape[xx] - x, 256});
          const bool pair =
              !planar() && (kind == Kind::Binary || kind == Kind::Fuzzy ||
                            kind == Kind::Independent);
          const ps_kernel_const_view_v8* b = nullptr;
          const ps_kernel_const_view_v8* mask = nullptr;
          if (pair || (active && kind == Kind::Affect)) {
            b = &r.view(1, 0, at);
            n = std::min(n, b->layout->origin[xx] + b->layout->shape[xx] - x);
          }
          const Coordinate ma{y, x};
          if (planar() && active) {
            mask = &r.view(kind == Kind::Affect ? 2U : 1U, 0, ma);
            n = std::min(n,
                         mask->layout->origin[1] + mask->layout->shape[1] - x);
          }
          work.poll(n);
          const auto* ap = static_cast<const std::uint8_t*>(a.allocation) +
                           offset(*a.layout, a.byte_offset, at);
          const auto* bp = b ? static_cast<const std::uint8_t*>(b->allocation) +
                                   offset(*b->layout, b->byte_offset, at)
                             : nullptr;
          const auto* mp =
              mask ? static_cast<const std::uint8_t*>(mask->allocation) +
                         offset(*mask->layout, mask->byte_offset, ma)
                   : nullptr;
          auto* op = static_cast<std::uint8_t*>(out.allocation) +
                     offset(l, out.byte_offset, at);
          const auto as = a.layout->strides[xx], os = l.strides[xx];
          const auto bs = b ? b->layout->strides[xx] : 0;
          const auto ms = mask ? mask->layout->strides[1] : 0;
          std::uint64_t i = 0;
#if defined(__aarch64__)
          if (active && as == sizeof(T) && os == sizeof(T) &&
              (!b || bs == sizeof(T)) && (!mask || ms == sizeof(T)) &&
              (kind == Kind::Invert || (kind == Kind::Fuzzy && mode < 3))) {
            if constexpr (sizeof(T) == 4) {
              for (; i + 4 <= n; i += 4) {
                const auto a4 =
                    vld1q_f32(reinterpret_cast<const float*>(ap) + i);
                float32x4_t v;
                if (kind == Kind::Invert) {
                  v = vsubq_f32(vdupq_n_f32(1), a4);
                } else {
                  const auto b4 =
                      vld1q_f32(reinterpret_cast<const float*>(bp) + i);
                  v = mode == 0   ? vbslq_f32(vcleq_f32(a4, b4), a4, b4)
                      : mode == 1 ? vbslq_f32(vcgeq_f32(a4, b4), a4, b4)
                                  : vabsq_f32(vsubq_f32(a4, b4));
                }
                vst1q_f32(reinterpret_cast<float*>(op) + i, v);
              }
            } else {
              for (; i + 2 <= n; i += 2) {
                const auto a2 =
                    vld1q_f64(reinterpret_cast<const double*>(ap) + i);
                float64x2_t v;
                if (kind == Kind::Invert) {
                  v = vsubq_f64(vdupq_n_f64(1), a2);
                } else {
                  const auto b2 =
                      vld1q_f64(reinterpret_cast<const double*>(bp) + i);
                  v = mode == 0   ? vbslq_f64(vcleq_f64(a2, b2), a2, b2)
                      : mode == 1 ? vbslq_f64(vcgeq_f64(a2, b2), a2, b2)
                                  : vabsq_f64(vsubq_f64(a2, b2));
                }
                vst1q_f64(reinterpret_cast<double*>(op) + i, v);
              }
            }
          }
#endif
          for (; i < n; ++i) {
            if (!active) {
              std::memcpy(op + os * static_cast<std::ptrdiff_t>(i),
                          ap + as * static_cast<std::ptrdiff_t>(i), sizeof(T));
              continue;
            }
            const auto av = kind == Kind::Nonzero
                                ? T(load<std::uint8_t>(ap, as, i))
                                : load<T>(ap, as, i);
            const auto bv = b ? load<T>(bp, bs, i) : T(0);
            const auto mv = mask ? load<T>(mp, ms, i) : T(0);
            save(op, os, i, evaluate(av, bv, mv, e, ratio, work));
          }
          x += n;
        }
      }
    }
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    Work work{inv, work_limit};
    SoftScratch* e = nullptr;
    Ratio* ratio = nullptr;
    const bool rational = kind == Kind::Soft || kind == Kind::SoftRange;
    if (rational) {
      e = new (inv.scratch[0].allocation) SoftScratch(work);
    }
    if (kind == Kind::Independent || kind == Kind::Affect)
      ratio = new (inv.scratch[0].allocation)
          Ratio(numeric_ops::SequenceProfile::Strict);
    struct Destroy {
      SoftScratch* e;
      Ratio* r;
      ~Destroy() {
        if (e)
          e->~SoftScratch();
        if (r)
          r->~Ratio();
      }
    } destroy{e, ratio};
    if (type == ElementType::Float32)
      rows<float>(kernel, inv, work, e, ratio);
    else
      rows<double>(kernel, inv, work, e, ratio);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_pointwise(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  const auto add = [&](const char* name, Kind kind, unsigned count,
                       std::vector<Parameter> p) {
    return registry->register_operation(definition(
        name, count, std::move(p), [kind](const auto& in, const auto& p) {
          return std::make_shared<Pointwise>(kind, in, p);
        }));
  };
  try {
    for (auto pair :
         {std::make_pair("binary_logic", Kind::Binary),
          std::make_pair("fuzzy_logic", Kind::Fuzzy),
          std::make_pair("independent_coverage", Kind::Independent)})
      check(add(pair.first, pair.second, 2, {{"operation", Type::String}}));
    check(add("invert", Kind::Invert, 1, {}));
    check(add("threshold", Kind::Threshold, 1,
              {{"threshold", Type::Float64}, {"comparison", Type::String}}));
    check(add("range_mask", Kind::Range, 1,
              {{"lower", Type::Float64},
               {"upper", Type::Float64},
               {"lower_closed", Type::Bool},
               {"upper_closed", Type::Bool}}));
    check(add("soft_threshold", Kind::Soft, 1,
              {{"threshold", Type::Float64},
               {"width", Type::Float64},
               {"curve", Type::String}}));
    check(add("soft_range", Kind::SoftRange, 1,
              {{"lower", Type::Float64},
               {"upper", Type::Float64},
               {"lower_width", Type::Float64},
               {"upper_width", Type::Float64},
               {"curve", Type::String}}));
    check(add("nonzero_to_binary", Kind::Nonzero, 1,
              {{"output_dtype", Type::String}}));
    check(add("affect_result", Kind::Affect, 3, {{"channels", Type::String}}));
    check(
        add("multiply_mask", Kind::Multiply, 2, {{"channels", Type::String}}));
    check(add(
        "apply_alpha", Kind::Alpha, 2,
        {{"alpha_channel", Type::Int64}, {"input_description", Type::String}}));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
