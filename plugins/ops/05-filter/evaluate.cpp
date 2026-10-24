#include <algorithm>
#include <cmath>
#include <new>
#include <type_traits>
#include <utility>

#include "05-filter/bounded_math.hpp"
#include "05-filter/common.hpp"
#include "05-filter/simd.hpp"

namespace ps::plugin_internal::filter {
namespace {
struct Atom {
  std::uint64_t raw;
  bool narrow = false;
  double value() const { return numeric_ops::numeric_double(raw, narrow); }
  bool zero() const {
    return (raw & (narrow ? UINT64_C(0x7fffffff)
                          : UINT64_C(0x7fffffffffffffff))) == 0;
  }
  bool nan() const {
    return narrow ? (raw & UINT64_C(0x7fffffff)) > UINT64_C(0x7f800000)
                  : (raw & UINT64_C(0x7fffffffffffffff)) >
                        UINT64_C(0x7ff0000000000000);
  }
};
struct WideArithmetic : BoundedArithmetic {
  explicit WideArithmetic(bool n) : BoundedArithmetic{n} {}
  BallValue from(Atom a) const { return BoundedArithmetic::from(a.value()); }
};
struct ExactArithmetic {
  Exact& e;
  ElementType output;
  std::optional<std::uint64_t> first_nan;
  Rational from(Atom a) {
    if (a.nan() && !first_nan) {
      first_nan = numeric_ops::converted_nan(
          a.raw, a.narrow ? ElementType::Float32 : ElementType::Float64,
          output);
    }
    return e.binary(a.raw, a.narrow);
  }
  Rational one() const { return e.integer(1); }
  Rational zero() const { return e.integer(0); }
  Rational add(const Rational& a, const Rational& b) const {
    return e.add(a, b);
  }
  Rational multiply(const Rational& a, const Rational& b) const {
    return e.multiply(a, b);
  }
  Rational divide(const Rational& a, const Rational& b) const {
    return e.divide(a, b);
  }
  Rational absolute(const Rational& a) const { return e.absolute(a); }
  bool is_zero(const Rational& a) const { return Exact::zero(a); }
  bool is_nan(const Rational& a) const { return Exact::nan(a); }
  std::optional<std::uint64_t> finish(const Rational& a) {
    check(e.status());
    if (first_nan) {
      return first_nan;
    }
    auto r = e.rounded(a, output == ElementType::Float32);
    check(e.status());
    return r;
  }
};
struct FastArithmetic {
  bool narrow;
  Profile profile;
  Fast from(Atom a) const { return Fast::point(a.value()); }
  Fast one() const { return Fast::point(1); }
  Fast zero() const { return Fast::point(0); }
  Fast add(Fast a, Fast b) const {
    if (a.low == a.high && b.low == b.high) {
      const double sum = a.low + b.low, part = sum - a.low;
      const double residual = (a.low - (sum - part)) + (b.low - part);
      if (std::isfinite(sum) && residual == 0) {
        return Fast::point(sum);
      }
    }
    return a + b;
  }
  Fast multiply(Fast a, Fast b) const {
    if (a.low == 1 && a.high == 1) {
      return b;
    }
    if (b.low == 1 && b.high == 1) {
      return a;
    }
    if (a.low == -1 && a.high == -1) {
      return -b;
    }
    if (b.low == -1 && b.high == -1) {
      return -a;
    }
    if (a.low == a.high && b.low == b.high) {
      const double product = a.low * b.low;
      // The residual cannot underflow in this range (at most 106 bits).
      if (std::isfinite(product) && std::abs(product) >= 0x1p-900 &&
          std::fma(a.low, b.low, -product) == 0) {
        return Fast::point(product);
      }
    }
    return a * b;
  }
  Fast divide(Fast a, Fast b) const {
    if (b.low == 1 && b.high == 1) {
      return a;
    }
    if (b.low == -1 && b.high == -1) {
      return -a;
    }
    return a / b;
  }
  // A certified point numerator/denominator permits a single hardware RN64
  // divide. For RN32, correct only a genuine binary32 midpoint using the exact
  // sign of the FMA remainder; the exponent guards exclude residual underflow.
  std::optional<std::uint64_t> quotient(Fast n, Fast d) const {
    if (n.low != n.high || d.low != d.high || !n.finite() || !d.finite() ||
        std::abs(n.low) < 0x1p-800 || std::abs(n.low) > 0x1p800 ||
        std::abs(d.low) < 0x1p-100 || std::abs(d.low) > 0x1p100) {
      return {};
    }
    const double q = n.low / d.low;
    if (!narrow) {
      return bits(q);
    }
    const double rounded =
        numeric_ops::numeric_double(generation::convert(q, true), true);
    double midpoint;
    if (std::isfinite(rounded)) {
      if (q == rounded) {
        return generation::convert(q, true);
      }
      const float adjacent = std::nextafter(static_cast<float>(rounded),
                                            q > rounded ? INFINITY : -INFINITY);
      const double next =
          std::isfinite(adjacent) ? adjacent : std::copysign(0x1p128, adjacent);
      midpoint = (rounded + next) * .5;
    } else {
      midpoint = std::copysign(0x1p128 - 0x1p103, q);
    }
    if (q == midpoint) {
      const double residual = std::fma(-q, d.low, n.low);
      if (residual != 0) {
        const bool up = (residual > 0) == (d.low > 0);
        return generation::convert(numeric_ops::numeric_neighbor(q, up), true);
      }
    }
    return generation::convert(q, true);
  }
  Fast absolute(Fast a) const {
    return a.low >= 0    ? a
           : a.high <= 0 ? -a
                         : Fast{0, std::max(-a.low, a.high)};
  }
  bool is_zero(Fast a) const { return a.low == 0 && a.high == 0; }
  bool is_nan(Fast a) const { return !a.finite(); }
  std::optional<std::uint64_t> finish(Fast a) const {
    if (!a.finite()) {
      return {};
    }
    const auto lo = generation::convert(a.low, narrow),
               hi = generation::convert(a.high, narrow);
    if (lo == hi) {
      return lo;
    }
    if (profile != Profile::Strict) {
      return a.accepted(a.low + (a.high - a.low) * .5, narrow);
    }
    return {};
  }
};
struct Evaluation {
  const Filter& f;
  const Reader& reader;
  const Fuel& fuel;
  const double* gx;
  const double* gy;
  bool narrow, separable, box;
  Fast fixed_divisor;
  Atom input(unsigned port, Coordinate at) const {
    if (port == 1 && f.broadcast) {
      at[f.component_axis] = 0;
    }
    fuel.poll();
    return {reader.raw(port, 0, at), narrow};
  }
  template <class Arithmetic>
  std::optional<std::uint64_t> sample(const Coordinate& at, unsigned output,
                                      Arithmetic& math) const {
    auto numerator = math.zero(), denominator = math.zero();
    bool first_n = true, first_d = true, participating = false;
    const bool color = f.kind == Kind::Color;
    const bool alpha = f.alpha_sample(at);
    const bool weighted =
        f.kind == Kind::Normalized || (color && f.alpha && !alpha);
    const bool cached_divisor = std::is_same_v<Arithmetic, FastArithmetic> &&
                                !weighted &&
                                f.geometry.boundary != Boundary::Truncate;
    if constexpr (std::is_same_v<Arithmetic, FastArithmetic>) {
      if (cached_divisor) {
        denominator = fixed_divisor;
      }
    }
    const auto& g = f.geometry;
    const auto cy = static_cast<std::int64_t>(at[g.ya]) + g.oy;
    const auto cx = static_cast<std::int64_t>(at[g.xa]) + g.ox;
    const auto low_y =
        cy + (g.correlation ? -static_cast<std::int64_t>(g.ay)
                            : static_cast<std::int64_t>(g.ay) -
                                  static_cast<std::int64_t>(g.kh) + 1);
    const auto low_x =
        cx + (g.correlation ? -static_cast<std::int64_t>(g.ax)
                            : static_cast<std::int64_t>(g.ax) -
                                  static_cast<std::int64_t>(g.kw) + 1);
    const bool interior = low_y >= 0 && low_x >= 0 &&
                          static_cast<std::uint64_t>(low_y) + g.kh <= g.h &&
                          static_cast<std::uint64_t>(low_x) + g.kw <= g.w;
    const auto add = [&](auto* sum, const auto& term, bool* first) {
      if (*first) {
        *sum = term;
        *first = false;
      } else {
        *sum = math.add(*sum, term);
      }
    };
    for (std::uint64_t j = 0; j < f.geometry.kh; ++j) {
      for (std::uint64_t i = 0; i < f.geometry.kw; ++i) {
        Atom a{bits(1)}, b{bits(1)};
        if (separable) {
          a.raw = bits(gy[j]);
          b.raw = bits(gx[i]);
        } else if (!box) {
          a.raw = bits(gx[j * g.kw + i]);
        }
        fuel.poll();
        if (a.zero() || b.zero()) {
          continue;
        }
        auto weight = math.from(a);
        if (separable) {
          weight = math.multiply(weight, math.from(b));
        }
        bool outside = false;
        auto pos = at;
        if (interior) {
          pos[g.ya] = g.correlation ? low_y + j : low_y + g.kh - 1 - j;
          pos[g.xa] = g.correlation ? low_x + i : low_x + g.kw - 1 - i;
        } else {
          pos = g.position(at, j, i, &outside);
        }
        if (outside && f.geometry.boundary == Boundary::Truncate) {
          continue;
        }
        Atom mask{bits(1)};
        if (f.kind == Kind::Normalized) {
          if (!outside) {
            mask = input(1, pos);
          }
          if (mask.zero()) {
            continue;
          }
        } else if (color && f.alpha) {
          auto alpha_pos = pos;
          alpha_pos[f.color_axis] = *f.alpha;
          mask = outside ? Atom{bits(f.geometry.cval)} : input(0, alpha_pos);
        }
        const bool has_alpha = color && f.alpha.has_value();
        auto coverage = weight;
        if (f.kind == Kind::Normalized || has_alpha) {
          coverage = math.multiply(weight, math.from(mask));
        }
        if (f.kind == Kind::Normalized) {
          participating = true;
        }
        // valid has no data dependency, including its empty branch.
        if (f.kind == Kind::Normalized && output == 1) {
          continue;
        }
        if (!cached_divisor && f.normalization != "none") {
          const auto divisor = weighted                  ? coverage
                               : f.normalization == "l1" ? math.absolute(weight)
                                                         : weight;
          add(&denominator, divisor, &first_d);
        }
        auto term = coverage;
        if (!alpha) {
          const auto value =
              outside ? Atom{bits(f.geometry.cval)} : input(0, pos);
          term = math.multiply(coverage, math.from(value));
        }
        add(&numerator, term, &first_n);
      }
    }
    if (f.kind == Kind::Normalized && !participating) {
      if (f.empty == "error") {
        throw Status{ErrorCode::OperationFailed,
                     "normalized convolution empty support",
                     DomainErrorCode{UINT64_C(0x46494c0001)}};
      }
      if (output == 1 || f.empty == "zero") {
        return UINT64_C(0);
      }
      return input(0, at).raw;
    }
    if (f.kind == Kind::Normalized && output == 1) {
      return UINT64_C(1);
    }
    if (color && !alpha && math.is_zero(denominator) &&
        !math.is_nan(numerator)) {
      return UINT64_C(0);
    }
    if constexpr (std::is_same_v<Arithmetic, FastArithmetic>) {
      const auto d = f.normalization == "none" ? math.one() : denominator;
      auto n = numerator;
      if (f.kind == Kind::Convolve || f.kind == Kind::Correlate ||
          f.kind == Kind::Separable || f.kind == Kind::Correlation) {
        n = math.add(n, math.multiply(math.from({bits(f.bias)}), d));
      }
      if (auto rounded = math.quotient(n, d)) {
        return rounded;
      }
    }
    auto value = numerator;
    if (f.normalization != "none") {
      value = math.divide(numerator, denominator);
    }
    if (f.kind == Kind::Convolve || f.kind == Kind::Correlate ||
        f.kind == Kind::Separable || f.kind == Kind::Correlation) {
      value = math.add(value, math.from({bits(f.bias)}));
    }
    return math.finish(value);
  }

  std::optional<std::uint64_t> refined(const Coordinate& at, unsigned port,
                                       generation::Math& exact_math) const {
    WideArithmetic wide{narrow};
    auto result = sample(at, port, wide);
    if (!result) {
      ExactArithmetic exact{exact_math.exact, f.type, {}};
      result = sample(at, port, exact);
    }
    return result;
  }
  bool tiled(const ps_kernel_mutable_view_v8& output, double* image,
             generation::Math& exact_math, unsigned port) const {
    if (separable || box || (f.kind == Kind::Normalized && port == 1) ||
        (!narrow && f.profile == Profile::Strict)) {
      return false;
    }
    const auto& g = f.geometry;
    const auto count = g.kh * g.kw;
    const auto bounded = [](double x) {
      return x == 0 || (std::abs(x) >= 0x1p-100 && std::abs(x) <= 0x1p100);
    };
    bool any = false;
    for (std::uint64_t i = 0; i < count; ++i) {
      fuel.poll();
      if (!bounded(gx[i])) {
        return false;
      }
      any = any || gx[i] != 0;
    }
    if (!any || count > (UINT64_C(1) << 40)) {
      return false;
    }
    const auto& layout = *output.layout;
    const auto height = layout.shape[g.ya], width = layout.shape[g.xa];
    const auto rows = height + g.kh - 1, pitch = width + g.kw - 1;
    auto* coverage = image + rows * pitch;
    const std::int64_t low_y =
        static_cast<std::int64_t>(layout.origin[g.ya]) + g.oy +
        (g.correlation ? -static_cast<std::int64_t>(g.ay)
                       : static_cast<std::int64_t>(g.ay) -
                             static_cast<std::int64_t>(g.kh) + 1);
    const std::int64_t low_x =
        static_cast<std::int64_t>(layout.origin[g.xa]) + g.ox +
        (g.correlation ? -static_cast<std::int64_t>(g.ax)
                       : static_cast<std::int64_t>(g.ax) -
                             static_cast<std::int64_t>(g.kw) + 1);
    Coordinate plane{};
    std::uint64_t planes = 1;
    for (unsigned a = 0; a < layout.rank; ++a) {
      plane[a] = layout.origin[a];
      if (a != g.ya && a != g.xa) {
        planes *= layout.shape[a];
      }
    }
    FastArithmetic fast{narrow, f.profile};
    // Up to two products and one addition per term. This also covers rounding
    // in the nonnegative magnitude sum: gamma_(3A)/(1-gamma_(3A)) <
    // (8A+16)*2^-52 for A<=2^40. Bounded factors exclude all binary64
    // under/overflow.
    const double factor = static_cast<double>(8 * count + 16) * 0x1p-52;
    const auto enclose = [&](double center, double magnitude) {
      if (!std::isfinite(center) || !std::isfinite(magnitude)) {
        return Fast{-INFINITY, INFINITY};
      }
      const auto radius = numeric_ops::numeric_up(magnitude * factor);
      return Fast{numeric_ops::numeric_down(center - radius),
                  numeric_ops::numeric_up(center + radius)};
    };
    for (std::uint64_t pi = 0; pi < planes; ++pi) {
      const bool chosen = f.selected(plane);
      const bool normalized = f.kind == Kind::Normalized;
      const bool alpha = f.alpha_sample(plane);
      const bool weighted =
          normalized || (f.kind == Kind::Color && f.alpha && !alpha);
      if (chosen) {
        for (std::uint64_t y = 0; y < rows; ++y) {
          const auto my = g.map(static_cast<__int128>(low_y) + y, g.h);
          for (std::uint64_t x = 0; x < pitch; ++x) {
            fuel.poll();
            const auto mx = g.map(static_cast<__int128>(low_x) + x, g.w);
            const bool outside = my < 0 || mx < 0;
            auto at = plane;
            if (!outside) {
              at[g.ya] = my;
              at[g.xa] = mx;
            }
            double m = 1, value = g.cval;
            if (weighted) {
              if (normalized) {
                m = outside ? (g.boundary == Boundary::Truncate ? 0 : 1)
                            : input(1, at).value();
              } else {
                auto ap = at;
                ap[f.color_axis] = *f.alpha;
                m = outside ? g.cval : input(0, ap).value();
              }
            }
            if (normalized && m == 0) {
              value = 0;
            } else if (!outside) {
              value = input(0, at).value();
            }
            const auto index = y * pitch + x;
            image[index] = bounded(value) ? value : NAN;
            coverage[index] = bounded(m) ? m : NAN;
          }
        }
      }
      for (std::uint64_t y = 0; y < height; ++y) {
        for (std::uint64_t x = 0; x < width; x += 2) {
          Pair sum(0), magnitude(0), denominator(0), den_magnitude(0);
          const auto lanes = std::min(UINT64_C(2), width - x);
          // A partial pair uses the identical arithmetic and final candidate.
          // Its unused lane never reads beyond the admitted halo.
          const auto load_pair = [&](const double* p) {
            return lanes == 2 ? Pair::load(p) : Pair::single(*p);
          };
          if (chosen) {
            for (std::uint64_t j = 0; j < g.kh; ++j) {
              const auto row = y + (g.correlation ? j : g.kh - 1 - j);
              for (std::uint64_t i = 0; i < g.kw; ++i) {
                fuel.poll(16);
                const double coefficient = gx[j * g.kw + i];
                if (coefficient == 0) {
                  continue;
                }
                const auto index =
                    row * pitch + x + (g.correlation ? i : g.kw - 1 - i);
                Pair weight(coefficient);
                if (weighted) {
                  weight = weight * load_pair(coverage + index);
                  denominator = denominator + weight;
                  den_magnitude = den_magnitude + weight.absolute();
                }
                const auto term = weight * load_pair(image + index);
                sum = sum + term;
                magnitude = magnitude + term.absolute();
              }
            }
          }
          double sums[2], magnitudes[2], denominators[2], den_magnitudes[2];
          sum.save(sums);
          magnitude.save(magnitudes);
          denominator.save(denominators);
          den_magnitude.save(den_magnitudes);
          for (std::uint64_t lane = 0; lane < lanes; ++lane) {
            auto at = plane;
            at[g.ya] += y;
            at[g.xa] += x + lane;
            if (!chosen) {
              fuel.poll();
              store(output, at, reader.raw(0, 0, at));
              continue;
            }
            std::optional<std::uint64_t> result;
            {
              auto value = enclose(sums[lane], magnitudes[lane]);
              if (f.normalization != "none") {
                value = fast.divide(
                    value,
                    weighted ? enclose(denominators[lane], den_magnitudes[lane])
                             : fixed_divisor);
              }
              if (f.kind == Kind::Convolve || f.kind == Kind::Correlate) {
                value = fast.add(value, Fast::point(f.bias));
              }
              if (value.nonzero()) {
                result = fast.finish(value);
              }
            }
            if (!result) {
              result = sample(at, port, fast);
            }
            if (!result) {
              result = refined(at, port, exact_math);
            }
            store(output, at, *result);
          }
        }
      }
      for (unsigned a = layout.rank; a-- > 0;) {
        if (a == g.ya || a == g.xa) {
          continue;
        }
        if (++plane[a] < layout.origin[a] + layout.shape[a]) {
          break;
        }
        plane[a] = layout.origin[a];
      }
    }
    return true;
  }

  // Intermediate entries enclose exact horizontal sums; no dtype rounding is
  // introduced by the separable pass. Uncertain/special outputs use sample().
  bool rows(const ps_kernel_mutable_view_v8& output, Fast* temporary,
            generation::Math& exact_math) const {
    if (!separable && !box) {
      return false;
    }
    const auto& g = f.geometry;
    const auto& layout = *output.layout;
    const auto height = layout.shape[g.ya], width = layout.shape[g.xa];
    const auto rows = height + g.kh - 1;
    auto* line = reinterpret_cast<double*>(temporary + rows * width);
    const std::int64_t low_y =
        static_cast<std::int64_t>(layout.origin[g.ya]) + g.oy +
        (g.correlation ? -static_cast<std::int64_t>(g.ay)
                       : static_cast<std::int64_t>(g.ay) -
                             static_cast<std::int64_t>(g.kh) + 1);
    const std::int64_t low_x =
        static_cast<std::int64_t>(layout.origin[g.xa]) + g.ox +
        (g.correlation ? -static_cast<std::int64_t>(g.ax)
                       : static_cast<std::int64_t>(g.ax) -
                             static_cast<std::int64_t>(g.kw) + 1);
    const bool truncate = g.boundary == Boundary::Truncate;
    // Sliding-window reset points may widen enclosures differently. Require
    // unique final rounding for box outputs, including accelerated profiles.
    FastArithmetic fast{narrow, box ? Profile::Strict : f.profile};
    Coordinate plane{};
    std::uint64_t planes = 1;
    for (unsigned a = 0; a < layout.rank; ++a) {
      plane[a] = layout.origin[a];
      if (a != g.ya && a != g.xa) {
        planes *= layout.shape[a];
      }
    }
    const auto count_axis = [](std::int64_t first, std::uint64_t n,
                               std::uint64_t bound) {
      const auto low = std::max(INT64_C(0), first);
      const auto high = std::min(static_cast<std::int64_t>(bound),
                                 first + static_cast<std::int64_t>(n));
      return static_cast<std::uint64_t>(std::max(INT64_C(0), high - low));
    };
    for (std::uint64_t plane_index = 0; plane_index < planes; ++plane_index) {
      const auto load = [&](std::int64_t y, std::int64_t x, bool* present) {
        fuel.poll();
        const auto my = g.map(y, g.h), mx = g.map(x, g.w);
        *present = !(truncate && (my < 0 || mx < 0));
        if (my < 0 || mx < 0) {
          return Fast::point(truncate ? 0 : g.cval);
        }
        auto pos = plane;
        pos[g.ya] = my;
        pos[g.xa] = mx;
        return fast.from(input(0, pos));
      };
      for (std::uint64_t r = 0; r < rows; ++r) {
        for (std::uint64_t x = 0; x < width + g.kw - 1; ++x) {
          bool present;
          line[x] = load(low_y + r, low_x + x, &present).low;
        }
        Fast horizontal = fast.zero();
        for (std::uint64_t x = 0; x < width; ++x) {
          if (!box || !x) {
            horizontal = fast.zero();
            bool first = true;
            for (std::uint64_t i = 0; i < g.kw; ++i) {
              fuel.poll();
              const double coefficient = box ? 1 : gx[i];
              if (coefficient == 0) {
                continue;
              }
              const auto sample_x = x + (g.correlation ? i : g.kw - 1 - i);
              const auto value = Fast::point(line[sample_x]);
              const auto term = fast.multiply(Fast::point(coefficient), value);
              horizontal = first ? term : fast.add(horizontal, term);
              first = false;
            }
          } else {
            fuel.poll(2);
            horizontal = fast.add(horizontal, -Fast::point(line[x - 1]));
            horizontal = fast.add(horizontal, Fast::point(line[x + g.kw - 1]));
          }
          temporary[r * width + x] = horizontal;
        }
      }
      for (std::uint64_t x = 0; x < width; ++x) {
        Fast vertical = fast.zero();
        for (std::uint64_t y = 0; y < height; ++y) {
          if (!box || !y) {
            vertical = fast.zero();
            bool first = true;
            for (std::uint64_t j = 0; j < g.kh; ++j) {
              fuel.poll();
              const auto r = y + (g.correlation ? j : g.kh - 1 - j);
              const double coefficient = box ? 1 : gy[j];
              if (coefficient == 0 ||
                  (truncate &&
                   g.map(static_cast<__int128>(low_y) + r, g.h) < 0)) {
                continue;
              }
              const auto term = fast.multiply(Fast::point(coefficient),
                                              temporary[r * width + x]);
              vertical = first ? term : fast.add(vertical, term);
              first = false;
            }
          } else {
            fuel.poll(2);
            if (!truncate ||
                g.map(static_cast<__int128>(low_y) + y - 1, g.h) >= 0) {
              vertical = fast.add(vertical, -temporary[(y - 1) * width + x]);
            }
            if (!truncate ||
                g.map(static_cast<__int128>(low_y) + y + g.kh - 1, g.h) >= 0) {
              vertical =
                  fast.add(vertical, temporary[(y + g.kh - 1) * width + x]);
            }
          }
          auto divisor = fixed_divisor;
          if (truncate) {
            divisor = Fast::point(
                static_cast<double>(count_axis(low_y + y, g.kh, g.h) *
                                    count_axis(low_x + x, g.kw, g.w)));
          }
          if (f.normalization == "none") {
            divisor = fast.one();
          }
          auto numerator = vertical;
          const bool biased =
              f.kind == Kind::Separable || f.kind == Kind::Correlation;
          if (biased) {
            numerator = fast.add(numerator,
                                 fast.multiply(Fast::point(f.bias), divisor));
          }
          auto result = fast.quotient(numerator, divisor);
          if (!result) {
            auto value = fast.divide(vertical, divisor);
            if (biased) {
              value = fast.add(value, Fast::point(f.bias));
            }
            // Reassociation may change the sign of exact zero.
            if (value.nonzero()) {
              result = fast.finish(value);
            }
          }
          auto at = plane;
          at[g.ya] += y;
          at[g.xa] += x;
          if (!result) {
            result = refined(at, 0, exact_math);
          }
          store(output, at, *result);
        }
      }
      for (unsigned a = layout.rank; a-- > 0;) {
        if (a == g.ya || a == g.xa) {
          continue;
        }
        if (++plane[a] < layout.origin[a] + layout.shape[a]) {
          break;
        }
        plane[a] = layout.origin[a];
      }
    }
    return true;
  }
};
}  // namespace
void Filter::run(const Kernel& kernel,
                 const ps_kernel_invocation_v8& inv) const {
  const Work work{inv, work_limit};
  const Fuel fuel{work};
  const Reader reader{kernel, inv};
  const auto& output = inv.outputs[0];
  bool copy_only =
      kind == Kind::Gaussian && geometry.kh == 1 && geometry.kw == 1;
  if (kind == Kind::Color && color_axis >= 0) {
    const auto first = output.layout->origin[color_axis];
    const auto last = first + output.layout->shape[color_axis];
    copy_only = true;
    for (auto c : color_channels) {
      if (c >= first && c < last) {
        copy_only = false;
      }
    }
    if (alpha && *alpha >= first && *alpha < last) {
      copy_only = false;
    }
  }
  if (copy_only) {
    samples(output, [&](const Coordinate& at, std::uint64_t) {
      fuel.poll();
      store(output, at, reader.raw(0, 0, at));
    });
    fuel.flush();
    return;
  }
  generation::MathArena arena(inv.scratch[0].allocation, work);
  auto& math = *arena;
  const auto consume = [&fuel](std::uint64_t n) { return fuel(n); };
  math.exact.bind(consume);
  math.d.consume = core_internal::WorkConsumer(fuel);
  auto* gx = reinterpret_cast<double*>(arena.value + 1);
  auto* gy = separable() ? gx + geometry.kw : gx;
  if (kind == Kind::Gaussian) {
    for (std::uint64_t i = 0; i <= geometry.kw / 2; ++i) {
      fuel.poll();
      gx[geometry.kw / 2 - i] = gx[geometry.kw / 2 + i] =
          number(gaussian_coefficient(math, sx, i));
    }
    for (std::uint64_t j = 0; j <= geometry.kh / 2; ++j) {
      fuel.poll();
      gy[geometry.kh / 2 - j] = gy[geometry.kh / 2 + j] =
          number(gaussian_coefficient(math, sy, j));
    }
  } else if (separable()) {
    for (std::uint64_t i = 0; i < geometry.kw; ++i) {
      fuel.poll();
      gx[i] = number(reader.raw(1, 0, {i}));
    }
    for (std::uint64_t j = 0; j < geometry.kh; ++j) {
      fuel.poll();
      gy[j] = number(reader.raw(2, 0, {j}));
    }
  } else if (!box()) {
    const unsigned port = kind == Kind::Normalized ? 2 : 1;
    for (std::uint64_t j = 0; j < geometry.kh; ++j) {
      for (std::uint64_t i = 0; i < geometry.kw; ++i) {
        fuel.poll();
        gx[j * geometry.kw + i] = number(reader.raw(port, 0, {j, i}));
      }
    }
  }
  const bool narrow = type == ElementType::Float32;
  Fast divisor = Fast::point(0);
  if (box()) {
    const auto area = static_cast<unsigned __int128>(geometry.kh) * geometry.kw;
    const auto rounded = static_cast<double>(area);
    divisor = static_cast<unsigned __int128>(rounded) == area
                  ? Fast::point(rounded)
                  : Fast{numeric_ops::numeric_down(rounded),
                         numeric_ops::numeric_up(rounded)};
  } else if (normalization != "none" && kind != Kind::Normalized) {
    FastArithmetic fast{narrow, profile};
    bool first = true;
    for (std::uint64_t j = 0; j < geometry.kh; ++j) {
      for (std::uint64_t i = 0; i < geometry.kw; ++i) {
        fuel.poll();
        const auto a = separable() ? gy[j]
                       : box()     ? 1
                                   : gx[j * geometry.kw + i];
        const auto b = separable() ? gx[i] : 1;
        if (a == 0 || b == 0) {
          continue;
        }
        auto weight = fast.multiply(Fast::point(a), Fast::point(b));
        if (normalization == "l1") {
          weight = fast.absolute(weight);
        }
        divisor = first ? weight : fast.add(divisor, weight);
        first = false;
      }
    }
  }
  const Evaluation evaluator{*this,  reader,      fuel,  gx,     gy,
                             narrow, separable(), box(), divisor};
  const auto coefficient_count = box()         ? 0
                                 : separable() ? geometry.kh + geometry.kw
                                               : geometry.kh * geometry.kw;
  auto* temporary = reinterpret_cast<Fast*>(gx + coefficient_count);
  if (evaluator.tiled(output, reinterpret_cast<double*>(temporary), math,
                      kernel.writes[0].first) ||
      evaluator.rows(output, temporary, math)) {
    fuel.flush();
    return;
  }
  samples(output, [&](const Coordinate& at, std::uint64_t) {
    if (!selected(at)) {
      fuel.poll();
      store(output, at, reader.raw(0, 0, at));
      return;
    }
    FastArithmetic fast{narrow, profile};
    auto result = evaluator.sample(at, kernel.writes[0].first, fast);
    if (!result) {
      result = evaluator.refined(at, kernel.writes[0].first, math);
    }
    store(output, at, *result);
  });
  fuel.flush();
}
}  // namespace ps::plugin_internal::filter
