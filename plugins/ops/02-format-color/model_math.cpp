#include "02-format-color/model_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace ps::plugin_internal::model_ops {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::numeric_bits;
using numeric_ops::numeric_double;
using numeric_ops::numeric_down;
using numeric_ops::numeric_up;
using numeric_ops::SequenceProfile;
Status domain(const char* message) {
  return {ErrorCode::OperationFailed,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Domain, FailureScope::Atom}};
}
Fast interval(RationalMath& math, const Rational& value) {
  if (RationalMath::zero(value))
    return Fast::point(value.negative ? -0.0 : 0.0);
  const auto bits = math.rounded(value, false);
  const auto number = numeric_double(bits);
  if (!std::isfinite(number))
    return {-INFINITY, INFINITY};
  const auto direction = math.order(value, math.binary(bits));
  return {direction < 0 ? numeric_down(number) : number,
          direction > 0 ? numeric_up(number) : number};
}
Matrix inverse(RationalMath& math, const Matrix& a) {
  Matrix result;
  Rational determinant = math.integer(0);
  for (unsigned column = 0; column < 3; ++column) {
    const unsigned j = (column + 1) % 3, k = (column + 2) % 3;
    determinant =
        math.add(determinant,
                 math.multiply(a[0][column],
                               math.subtract(math.multiply(a[1][j], a[2][k]),
                                             math.multiply(a[1][k], a[2][j]))));
  }
  if (RationalMath::zero(determinant))
    throw Status{ErrorCode::Internal, "FMT-11 singular fixed matrix"};
  for (unsigned row = 0; row < 3; ++row)
    for (unsigned column = 0; column < 3; ++column) {
      const unsigned i = (column + 1) % 3, j = (column + 2) % 3;
      const unsigned k = (row + 1) % 3, l = (row + 2) % 3;
      result[row][column] =
          math.divide(math.subtract(math.multiply(a[i][k], a[j][l]),
                                    math.multiply(a[i][l], a[j][k])),
                      determinant);
    }
  return result;
}
std::array<unsigned, 3> extrema(const std::array<std::uint64_t, 3>& bits,
                                bool narrow) {
  unsigned minimum = 0, maximum = 0;
  for (unsigned i = 1; i < 3; ++i) {
    const auto current = BinaryParts::decode(bits[i], narrow);
    const auto low = BinaryParts::decode(bits[minimum], narrow);
    const auto high = BinaryParts::decode(bits[maximum], narrow);
    if (current.order_key() < low.order_key() ||
        (!current.magnitude && !low.magnitude && current.negative))
      minimum = i;
    if (current.order_key() > high.order_key() ||
        (!current.magnitude && !high.magnitude && !current.negative))
      maximum = i;
  }
  // Sector priority R,G,B is independent of numeric signed-zero selection.
  unsigned sector = 0;
  const auto high = BinaryParts::decode(bits[maximum], narrow).order_key();
  while (sector < 2 &&
         BinaryParts::decode(bits[sector], narrow).order_key() != high)
    ++sector;
  return {minimum, maximum, sector};
}
Fast fast_abs(Fast value) {
  if (value.low >= 0)
    return value;
  if (value.high <= 0)
    return -value;
  return {0, std::max(-value.low, value.high)};
}
std::optional<Fast> fast_root(Fast value) {
  if (!numeric_ops::accelerated_math_available() || !value.finite() ||
      std::abs(value.low) > 0x1p100 || std::abs(value.high) > 0x1p100 ||
      (value.low != 0 && std::abs(value.low) < 0x1p-100) ||
      (value.high != 0 && std::abs(value.high) < 0x1p-100))
    return {};
  const double inputs[2]{value.low, value.high};
  double outputs[2]{};
  photospider_sleef_evaluate(12, inputs, nullptr, outputs, 2);
  const auto low = value.low == 0
                       ? Fast::point(0)
                       : numeric_ops::accelerated_math_enclosure(outputs[0]);
  const auto high = value.high == 0
                        ? Fast::point(0)
                        : numeric_ops::accelerated_math_enclosure(outputs[1]);
  return Fast{low.low, high.high};
}
}  // namespace

const char* operation_name(Kind kind) {
  static constexpr const char* names[]{
      "color.xyz_to_cielab",    "color.cielab_to_xyz",
      "color.cielab_to_cielch", "color.cielch_to_cielab",
      "color.xyz_to_oklab",     "color.oklab_to_xyz",
      "color.oklab_to_oklch",   "color.oklch_to_oklab",
      "color.rgb_to_hsl",       "color.hsl_to_rgb",
      "color.rgb_to_hsv",       "color.hsv_to_rgb",
      "color.rgb_to_ycbcr_ncl", "color.ycbcr_ncl_to_rgb",
      "color.xyz_to_xyy",       "color.xyy_to_xyz",
      "color.color_to_gray",    "color.gray_to_color",
      "mask.threshold_channel", "color.black_white_to_gray"};
  return names[static_cast<unsigned>(kind)];
}
const char* source_model(Kind kind, GrayKind gray) {
  static constexpr const char* names[]{
      "xyz",   "cielab", "cielab", "cielch", "xyz",  "oklab",      "oklab",
      "oklch", "rgb",    "hsl",    "rgb",    "hsv",  "rgb",        "ycbcr",
      "xyz",   "xyy",    "",       "gray",   "gray", "black_white"};
  if (kind == Kind::ColorToGray) {
    static constexpr const char* models[]{"xyz", "ycbcr", "cielab", "oklab"};
    return models[static_cast<unsigned>(gray)];
  }
  return names[static_cast<unsigned>(kind)];
}
const char* target_model(Kind kind, GrayKind gray) {
  static constexpr const char* names[]{
      "cielab", "xyz", "cielch", "cielab", "oklab",       "xyz",   "oklch",
      "oklab",  "hsl", "rgb",    "hsv",    "rgb",         "ycbcr", "rgb",
      "xyy",    "xyz", "gray",   "",       "black_white", "gray"};
  if (kind == Kind::GrayToColor)
    return source_model(Kind::ColorToGray, gray);
  return names[static_cast<unsigned>(kind)];
}
unsigned support(Kind kind, unsigned c, GrayKind gray) {
  static constexpr unsigned masks[20][3]{
      {2, 3, 6}, {3, 1, 5}, {1, 6, 6}, {1, 6, 6}, {7, 7, 7},
      {7, 7, 7}, {1, 6, 6}, {1, 6, 6}, {7, 7, 7}, {7, 7, 7},
      {7, 7, 7}, {7, 7, 7}, {7, 7, 7}, {5, 7, 3}, {7, 7, 2},
      {7, 4, 7}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
  if (kind == Kind::ColorToGray)
    return gray == GrayKind::LinearY ? 2 : 1;
  if (kind == Kind::GrayToColor && gray == GrayKind::LinearY)
    return 1;
  return masks[static_cast<unsigned>(kind)][c];
}
int copied_component(Kind kind, unsigned c, GrayKind gray) {
  if ((kind == Kind::LabToLch || kind == Kind::LchToLab ||
       kind == Kind::OklabToOklch || kind == Kind::OklchToOklab) &&
      c == 0)
    return 0;
  if (kind == Kind::XyzToXyy && c == 2)
    return 1;
  if (kind == Kind::XyyToXyz && c == 1)
    return 2;
  if (kind == Kind::ColorToGray)
    return gray == GrayKind::LinearY ? 1 : 0;
  if (kind == Kind::GrayToColor) {
    if (gray == GrayKind::LinearY)
      return c == 1 ? 0 : -1;
    return c == 0 ? 0 : -2;
  }
  return -1;
}
Result<std::shared_ptr<const Constants>> prepare_constants(
    const MathConfig& config) {
  using Answer = Result<std::shared_ptr<const Constants>>;
  try {
    input_internal::Float32Environment environment;
    const RationalMath::Work work = [](std::uint64_t) {
      return Status::success();
    };
    RationalMath math(SequenceProfile::Strict);
    math.bind(work);
    auto result = std::make_shared<Constants>();
    auto& out = *result;
    out.delta = math.fraction(6, 29);
    out.epsilon = math.cube(out.delta);
    out.delta_fast = interval(math, out.delta);
    out.epsilon_fast = interval(math, out.epsilon);
    for (unsigned i = 0; i < 2; ++i) {
      out.white_xy[i] = math.binary(numeric_bits(config.white[i]));
      out.white_fast[i] = Fast::point(config.white[i]);
    }
    out.white_xz[0] = math.divide(out.white_xy[0], out.white_xy[1]);
    out.white_xz[1] = math.divide(
        math.subtract(math.subtract(math.integer(1), out.white_xy[0]),
                      out.white_xy[1]),
        out.white_xy[1]);
    for (unsigned i = 0; i < 2; ++i)
      out.white_xz_fast[i] = interval(math, out.white_xz[i]);
    if (config.kind == Kind::XyzToOklab || config.kind == Kind::OklabToXyz) {
      static constexpr const char* m1[3][3]{
          {"0.8190224379967030", "0.3619062600528904", "-0.1288737815209879"},
          {"0.0329836539323885", "0.9292868615863434", "0.0361446663506424"},
          {"0.0481771893596242", "0.2642395317527308", "0.6335478284694309"}};
      static constexpr const char* m2[3][3]{
          {"0.2104542683093140", "0.7936177747023054", "-0.0040720430116193"},
          {"1.9779985324311684", "-2.4285922420485799", "0.4505937096174110"},
          {"0.0259040424655478", "0.7827717124575296", "-0.8086757549230774"}};
      for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
          out.first[i][j] = math.decimal(m1[i][j]);
          out.second[i][j] = math.decimal(m2[i][j]);
        }
      if (config.kind == Kind::OklabToXyz) {
        auto first = inverse(math, out.second);
        out.second = inverse(math, out.first);
        out.first = std::move(first);
      }
    } else if (config.kind == Kind::RgbToYcbcr ||
               config.kind == Kind::YcbcrToRgb) {
      const auto one = math.integer(1), two = math.integer(2);
      const auto kr = math.binary(numeric_bits(config.ncl[0]));
      const auto kb = math.binary(numeric_bits(config.ncl[1]));
      const auto kg = math.subtract(math.subtract(one, kr), kb);
      const auto dr = math.multiply(two, math.subtract(one, kr));
      const auto db = math.multiply(two, math.subtract(one, kb));
      if (config.kind == Kind::RgbToYcbcr) {
        out.first[0] = {kr, kg, kb};
        out.first[1] = {math.negate(math.divide(kr, db)),
                        math.negate(math.divide(kg, db)), math.fraction(1, 2)};
        out.first[2] = {math.fraction(1, 2), math.negate(math.divide(kg, dr)),
                        math.negate(math.divide(kb, dr))};
      } else {
        out.first[0] = {one, math.integer(0), dr};
        out.first[1] = {one,
                        math.negate(math.divide(math.multiply(kb, db), kg)),
                        math.negate(math.divide(math.multiply(kr, dr), kg))};
        out.first[2] = {one, db, math.integer(0)};
      }
    }
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < 3; ++j) {
        out.first_fast[i][j] = interval(math, out.first[i][j]);
        out.second_fast[i][j] = interval(math, out.second[i][j]);
      }
    return Answer(std::move(result));
  } catch (const Status& status) {
    return Answer(status);
  }
}
ModelMath::ModelMath(const MathConfig& config, const Constants& constants,
                     const RationalMath::Work& work)
    : config_(config),
      constants_(constants),
      work_(work),
      exact_(config.profile),
      directed_(config.profile),
      certified_(config.algorithm == Algorithm::Reference
                     ? SequenceProfile::Strict
                     : config.profile) {
  exact_.bind(work_);
  directed_.functions.math.consume = &work_;
}
Rational ModelMath::dot(const std::array<Rational, 3>& row,
                        const std::array<Rational, 3>& input, unsigned mask) {
  auto result = exact_.integer(0);  // NUM dot's explicit +0 bias.
  for (unsigned i = 0; i < 3; ++i)
    if (mask & (1U << i))
      result = exact_.add(result, exact_.multiply(row[i], input[i]));
  return result;
}
ModelMath::RootTerm ModelMath::root(const Rational& value) {
  return {value, exact_.perfect_cube_root(value)};
}
ModelMath::RootTerm ModelMath::cie_f(const Rational& value) {
  if (!RationalMath::nan(value) && exact_.order(value, constants_.epsilon) > 0)
    return root(value);
  return {value, exact_.add(exact_.multiply(exact_.fraction(841, 108), value),
                            exact_.fraction(4, 29))};
}
Rational ModelMath::cie_g(const Rational& value) {
  if (!RationalMath::nan(value) && exact_.order(value, constants_.delta) > 0)
    return exact_.cube(value);
  return exact_.multiply(exact_.fraction(108, 841),
                         exact_.subtract(value, exact_.fraction(4, 29)));
}
void ModelMath::enclose_root(Interval result, const RootTerm& term) {
  auto& math = directed_.functions.math;
  if (term.exact) {
    exact_.enclose(math, result, *term.exact);
    return;
  }
  Frame frame(math);
  auto base = math.interval(), exponent = math.interval();
  const int scale = exact_.normalized(math, base, term.argument);
  const int quotient = scale >= 0 ? scale / 3 : (scale - 2) / 3;
  math.scale(base, base, scale - 3 * quotient);
  math.integer(exponent, 1);
  math.divide_small(exponent, exponent, 3);
  directed_.power(result, base, exponent);
  math.scale(result, result, quotient);
  if (term.argument.negative)
    math.negate(result, result);
}
std::uint64_t ModelMath::root_sum(const std::array<RootTerm, 3>& roots,
                                  const std::array<Rational, 3>& weights,
                                  const Rational& bias) {
  auto classified = bias;
  bool all_exact = true, special = !RationalMath::finite(bias);
  for (unsigned i = 0; i < 3; ++i) {
    if (RationalMath::zero(weights[i]))
      continue;
    all_exact &= roots[i].exact.has_value();
    if (roots[i].exact) {
      special |= !RationalMath::finite(*roots[i].exact);
      classified =
          exact_.add(classified, exact_.multiply(weights[i], *roots[i].exact));
    }
  }
  if (all_exact || special)
    return exact_.rounded(classified, config_.narrow);
  auto& math = directed_.functions.math;
  for (unsigned precision = 128; precision <= 4096; precision *= 2) {
    ++counters_.refinements;
    exact_.work(1);
    math.precision = precision;
    Frame frame(math);
    try {
      auto result = math.interval(), factor = math.interval(),
           term = math.interval();
      exact_.enclose(math, result, bias);
      for (unsigned i = 0; i < 3; ++i) {
        if (RationalMath::zero(weights[i]))
          continue;
        enclose_root(term, roots[i]);
        exact_.enclose(math, factor, weights[i]);
        math.multiply(term, term, factor);
        math.add(result, result, term);
      }
      const auto low = math.rounded(result.low, config_.narrow);
      const auto high = math.rounded(result.high, config_.narrow);
      if (low == high)
        return low;
    } catch (const numeric_ops::DirectedInterval::Unresolved&) {
    }
  }
  RationalMath::capacity();
}
std::uint64_t ModelMath::times_pi(const Rational& value) {
  if (!RationalMath::finite(value) || RationalMath::zero(value))
    return exact_.rounded(value, config_.narrow);
  auto& math = directed_.functions.math;
  for (unsigned precision = 128; precision <= 4096; precision *= 2) {
    ++counters_.refinements;
    exact_.work(1);
    math.precision = precision;
    Frame frame(math);
    auto result = math.interval(), pi = math.interval();
    exact_.enclose(math, result, exact_.absolute(value));
    math.constant(pi, true);
    math.multiply(result, result, pi);
    const auto low = math.rounded(result.low, config_.narrow);
    const auto high = math.rounded(result.high, config_.narrow);
    if (low == high)
      return low |
             (value.negative ? UINT64_C(1) << (config_.narrow ? 31 : 63) : 0);
  }
  RationalMath::capacity();
}

std::uint64_t ModelMath::inverse_polar(const std::array<Rational, 3>& in,
                                       const std::array<std::uint64_t, 3>& bits,
                                       unsigned component) {
  const auto& chroma = in[1];
  const auto& hue = in[2];
  const bool sine = component == 2;
  const auto sign = UINT64_C(1) << (config_.narrow ? 31 : 63);
  if (!RationalMath::finite(hue))
    return exact_.rounded(exact_.invalid(), config_.narrow);
  if (config_.semantic && RationalMath::zero(chroma))
    return 0;
  // Algebraic quadrants must be evaluated before the interval path: neither
  // sinpi(integer) nor C*sqrt(1/2) is a rounded intermediate in this operation.
  bool landmark = RationalMath::zero(hue);
  unsigned quadrant = 0;
  if (config_.input_pi) {
    const auto turns = exact_.multiply(exact_.modulo(exact_.absolute(hue), 2),
                                       exact_.integer(4));
    quadrant = exact_.small_floor(turns);
    landmark = exact_.order(turns, exact_.integer(quadrant)) == 0;
  }
  if (landmark) {
    const bool zero = sine ? (quadrant == 0 || quadrant == 4)
                           : (quadrant == 2 || quadrant == 6);
    const bool negative =
        sine ? (zero ? hue.negative : ((quadrant > 4) != hue.negative))
             : (quadrant > 2 && quadrant < 6);
    if (zero) {
      auto z = exact_.integer(0);
      z.negative = negative;
      return exact_.rounded(exact_.multiply(chroma, z), config_.narrow);
    }
    if ((quadrant & 1) == 0) {
      auto unit = exact_.integer(1);
      unit.negative = negative;
      return exact_.rounded(exact_.multiply(chroma, unit), config_.narrow);
    }
    const auto square =
        exact_.divide(exact_.multiply(chroma, chroma), exact_.integer(2));
    return exact_.rounded_sqrt(square, config_.narrow) |
           ((chroma.negative != negative) ? sign : 0);
  }
  auto& math = directed_.functions.math;
  const auto decoded = BinaryParts::decode(bits[2], config_.narrow);
  for (unsigned precision = 128; precision <= 4096; precision *= 2) {
    ++counters_.refinements;
    exact_.work(1);
    math.precision = precision;
    Frame frame(math);
    try {
      auto s = math.interval(), c = math.interval();
      if (config_.input_pi) {
        // CertifiedMath::pi_pair uses its own interval pool, so perform the
        // exact dyadic reduction here and use this operation's admitted pool.
        auto reduced = math.interval(), angle = math.interval();
        auto r = exact_.modulo(exact_.absolute(hue), 2);
        const auto nearest = exact_.small_floor(exact_.add(
            exact_.multiply(r, exact_.integer(2)), exact_.fraction(1, 2)));
        r = exact_.subtract(r, exact_.fraction(nearest, 2));
        exact_.enclose(math, reduced, r);
        math.constant(angle, true);
        math.multiply(angle, angle, reduced);
        auto bs = math.interval(), bc = math.interval();
        directed_.functions.sincos_series(bs, bc, angle);
        directed_.functions.orient(s, c, bs, bc, nearest & 3, hue.negative);
      } else {
        directed_.functions.sincos(s, c, decoded);
      }
      auto trig = sine ? s : c;
      if (math.includes_zero(trig))
        continue;
      const bool negative = trig.low.negative;
      if (RationalMath::infinity(chroma))
        return exact_.rounded(exact_.inf(chroma.negative != negative),
                              config_.narrow);
      if (RationalMath::zero(chroma))
        return (chroma.negative != negative) ? sign : 0;
      auto factor = math.interval(), result = math.interval();
      exact_.enclose(math, factor, exact_.absolute(chroma));
      if (negative)
        math.negate(trig, trig);
      math.multiply(result, trig, factor);
      const auto low = math.rounded(result.low, config_.narrow);
      const auto high = math.rounded(result.high, config_.narrow);
      if (low == high)
        return low | ((chroma.negative != negative) ? sign : 0);
    } catch (const numeric_ops::DirectedInterval::Unresolved&) {
    }
  }
  RationalMath::capacity();
}

unsigned ModelMath::radian_sector(Interval q, std::uint64_t raw) {
  auto& math = directed_.functions.math;
  Frame frame(math);
  auto hue = BinaryParts::decode(raw, config_.narrow);
  if (!hue.magnitude) {
    math.integer(q, 0);
    return 0;
  }
  auto angle = math.interval(), pi = math.interval(), three = math.interval();
  math.dyadic(angle, hue.significand, hue.exponent);
  math.integer(three, 3);
  math.multiply(angle, angle, three);
  math.constant(pi, true);
  math.divide(q, angle, pi);
  auto& lower = math.number();
  auto& upper = math.number();
  math.shift(lower, q.low, -static_cast<int>(math.precision), true);
  math.shift(upper, q.high, -static_cast<int>(math.precision), true);
  if (math.compare(lower, upper) != 0)
    throw numeric_ops::DirectedInterval::Unresolved{};
  unsigned residue = 0;
  for (std::size_t i = numeric_ops::DirectedInterval::kWords; i-- > 0;)
    residue =
        static_cast<unsigned>(((static_cast<unsigned __int128>(residue) << 64) +
                               lower.magnitude.words[i]) %
                              6);
  auto whole = math.interval(), remainder = math.interval();
  math.shift(whole.low, lower, static_cast<int>(math.precision), true);
  math.copy(whole.high, whole.low);
  math.subtract(q, q, whole);
  math.integer(remainder, residue);
  math.add(q, q, remainder);
  if (hue.negative) {
    math.integer(remainder, 6);
    math.subtract(q, remainder, q);
    return 5 - residue;
  }
  return residue;
}

std::uint64_t ModelMath::inverse_hue(const std::array<Rational, 3>& in,
                                     const std::array<std::uint64_t, 3>& bits,
                                     unsigned component) {
  if (!RationalMath::finite(in[0]))
    return exact_.rounded(exact_.invalid(), config_.narrow);
  const auto one = exact_.integer(1), two = exact_.integer(2);
  const bool hsl = config_.kind == Kind::HslToRgb;
  const auto chroma =
      hsl ? exact_.multiply(
                exact_.subtract(one, exact_.absolute(exact_.subtract(
                                         exact_.multiply(two, in[2]), one))),
                in[1])
          : exact_.multiply(in[2], in[1]);
  const auto offset =
      exact_.subtract(in[2], hsl ? exact_.divide(chroma, two) : chroma);
  // Entries select c, v, or +0. Hue still has to be finite for a zero chroma.
  static constexpr unsigned selection[6][3]{{0, 1, 2}, {1, 0, 2}, {2, 0, 1},
                                            {2, 1, 0}, {1, 2, 0}, {0, 2, 1}};
  if (config_.input_pi) {
    const auto q = exact_.modulo(exact_.multiply(exact_.integer(3), in[0]), 6);
    const auto sector = exact_.small_floor(q);
    const auto index = selection[sector][component];
    auto contribution = exact_.integer(0);
    if (index == 0)
      contribution = chroma;
    if (index == 1)
      contribution = exact_.multiply(
          chroma, exact_.subtract(one, exact_.absolute(exact_.subtract(
                                           exact_.modulo(q, 2), one))));
    return exact_.rounded(exact_.add(contribution, offset), config_.narrow);
  }
  if (RationalMath::zero(chroma))
    return exact_.rounded(exact_.add(exact_.integer(0), offset),
                          config_.narrow);
  auto& math = directed_.functions.math;
  for (unsigned precision = 128; precision <= 4096; precision *= 2) {
    ++counters_.refinements;
    exact_.work(1);
    math.precision = precision;
    Frame frame(math);
    try {
      auto q = math.interval();
      const auto sector = radian_sector(q, bits[0]);
      const auto index = selection[sector][component];
      if (index != 1)
        return exact_.rounded(
            exact_.add(index == 0 ? chroma : exact_.integer(0), offset),
            config_.narrow);
      // On each open sector the triangle is an affine function of q.
      // Sector boundaries other than zero are irrational for finite binary H.
      auto term = math.interval(), c = math.interval(), base = math.interval();
      math.integer(term, (sector & 1) ? sector + 1 : sector);
      if (sector & 1)
        math.subtract(q, term, q);
      else
        math.subtract(q, q, term);
      if (!RationalMath::finite(chroma) || !RationalMath::finite(offset)) {
        // The triangle is positive in an open sector; exact H=0 has index!=1
        // for its nonzero component, but v=0 for G must retain Inf*0 => NaN.
        if (!BinaryParts::decode(bits[0], config_.narrow).magnitude)
          return exact_.rounded(
              exact_.add(exact_.multiply(chroma, exact_.integer(0)), offset),
              config_.narrow);
        return exact_.rounded(exact_.add(chroma, offset), config_.narrow);
      }
      exact_.enclose(math, c, chroma);
      exact_.enclose(math, base, offset);
      math.multiply(q, q, c);
      math.add(q, q, base);
      const auto low = math.rounded(q.low, config_.narrow);
      const auto high = math.rounded(q.high, config_.narrow);
      if (low == high)
        return low;
    } catch (const numeric_ops::DirectedInterval::Unresolved&) {
    }
  }
  RationalMath::capacity();
}

std::uint64_t ModelMath::reference(const std::array<std::uint64_t, 3>& bits,
                                   unsigned component) {
  std::array<Rational, 3> in;
  const auto mask = support(config_.kind, component, config_.gray);
  for (unsigned i = 0; i < 3; ++i)
    if (mask & (1u << i))
      in[i] = exact_.binary(bits[i], config_.narrow);
  const auto zero = exact_.integer(0), one = exact_.integer(1),
             two = exact_.integer(2);
  const auto finish = [&](const Rational& value) {
    return exact_.rounded(value, config_.narrow);
  };
  switch (config_.kind) {
    case Kind::XyzToLab: {
      const auto y = in[1];
      if (component == 0)
        return root_sum({cie_f(y), root(zero), root(zero)},
                        {exact_.fraction(29, 25), zero, zero},
                        exact_.fraction(-4, 25));
      const auto x =
          component == 1 ? exact_.divide(in[0], constants_.white_xz[0]) : y;
      const auto z =
          component == 1 ? y : exact_.divide(in[2], constants_.white_xz[1]);
      if (RationalMath::finite(x) && RationalMath::finite(z) &&
          exact_.order(x, z) == 0)
        return 0;
      const auto weight = exact_.integer(component == 1 ? 500 : 200);
      return root_sum({cie_f(x), cie_f(z), root(zero)},
                      {weight, exact_.negate(weight), zero}, zero);
    }
    case Kind::LabToXyz: {
      const auto fy =
          exact_.divide(exact_.add(exact_.multiply(exact_.integer(100), in[0]),
                                   exact_.integer(16)),
                        exact_.integer(116));
      if (component == 1)
        return finish(cie_g(fy));
      const auto t =
          component == 0
              ? exact_.add(fy, exact_.divide(in[1], exact_.integer(500)))
              : exact_.subtract(fy, exact_.divide(in[2], exact_.integer(200)));
      return finish(exact_.multiply(constants_.white_xz[component == 0 ? 0 : 1],
                                    cie_g(t)));
    }
    case Kind::LabToLch:
    case Kind::OklabToOklch:
      if (component == 1)
        return exact_.rounded_sqrt(exact_.add(exact_.multiply(in[1], in[1]),
                                              exact_.multiply(in[2], in[2])),
                                   config_.narrow);
      if (RationalMath::zero(in[1]) && RationalMath::zero(in[2]))
        return 0;
      {
        auto answer = certified_.evaluate(
            config_.output_pi ? numeric_ops::CertifiedKind::Atan2pi
                              : numeric_ops::CertifiedKind::Atan2,
            config_.narrow ? ElementType::Float32 : ElementType::Float64,
            bits[2], bits[1], work_, [] { return Status::success(); }, true);
        if (!answer.ok())
          throw answer.status();
        return answer.value();
      }
    case Kind::LchToLab:
    case Kind::OklchToOklab:
      return inverse_polar(in, bits, component);
    case Kind::XyzToOklab: {
      std::array<RootTerm, 3> roots;
      for (unsigned i = 0; i < 3; ++i)
        roots[i] = root(dot(constants_.first[i], in));
      return root_sum(roots, constants_.second[component], zero);
    }
    case Kind::OklabToXyz: {
      std::array<Rational, 3> cubes;
      for (unsigned i = 0; i < 3; ++i)
        cubes[i] = exact_.cube(dot(constants_.first[i], in));
      return finish(dot(constants_.second[component], cubes));
    }
    case Kind::RgbToYcbcr:
    case Kind::YcbcrToRgb:
      return finish(dot(constants_.first[component], in, mask));
    case Kind::XyzToXyy: {
      const auto total = exact_.add(exact_.add(in[0], in[1]), in[2]);
      if (config_.semantic && RationalMath::zero(total)) {
        if (RationalMath::zero(in[0]) && RationalMath::zero(in[1]) &&
            RationalMath::zero(in[2]))
          return finish(constants_.white_xy[component]);
        throw domain("XYZ to xyY has non-black zero tristimulus sum");
      }
      return finish(exact_.divide(in[component], total));
    }
    case Kind::XyyToXyz:
      if (config_.semantic && RationalMath::zero(in[1]))
        throw domain("xyY to XYZ requires nonzero chromaticity y");
      return finish(exact_.divide(
          exact_.multiply(
              component == 0
                  ? in[0]
                  : exact_.subtract(exact_.subtract(one, in[0]), in[1]),
              in[2]),
          in[1]));
    case Kind::RgbToHsl:
    case Kind::RgbToHsv: {
      const auto indices = extrema(bits, config_.narrow);
      const auto& minimum = in[indices[0]];
      const auto& maximum = in[indices[1]];
      const auto difference = exact_.subtract(maximum, minimum);
      const auto sum = exact_.add(maximum, minimum);
      const bool hsl = config_.kind == Kind::RgbToHsl;
      if (component == 2)
        return hsl ? finish(exact_.divide(sum, two)) : bits[indices[1]];
      if (RationalMath::zero(difference))
        return 0;
      if (component == 1) {
        const auto denominator =
            hsl ? exact_.subtract(one,
                                  exact_.absolute(exact_.subtract(sum, one)))
                : maximum;
        if (config_.semantic && RationalMath::zero(denominator))
          throw domain("non-gray RGB has a zero saturation denominator");
        return finish(exact_.divide(difference, denominator));
      }
      const auto sector = indices[2];
      const auto q = exact_.modulo(
          exact_.add(exact_.divide(exact_.subtract(in[(sector + 1) % 3],
                                                   in[(sector + 2) % 3]),
                                   difference),
                     exact_.integer(2 * sector)),
          6);
      const auto angle = exact_.divide(q, exact_.integer(3));
      return config_.output_pi ? finish(angle) : times_pi(angle);
    }
    case Kind::HslToRgb:
    case Kind::HsvToRgb:
      return inverse_hue(in, bits, component);
    case Kind::GrayToColor:
      return finish(
          exact_.multiply(in[0], constants_.white_xz[component == 0 ? 0 : 1]));
    case Kind::Threshold: {
      if (BinaryParts::decode(bits[0], config_.narrow).nan)
        return 0;
      return finish(exact_.integer(
          exact_.order(in[0], exact_.binary(numeric_bits(config_.threshold))) >=
                  0
              ? 1
              : 0));
    }
    case Kind::BinaryToGray:
    case Kind::ColorToGray:
      break;  // Selection/copy handling precedes arithmetic dispatch.
  }
  throw Status{ErrorCode::Internal, "FMT-11 unreachable arithmetic member"};
}

std::optional<Fast> ModelMath::fast(const std::array<std::uint64_t, 3>& bits,
                                    unsigned component) {
  std::array<Fast, 3> in;
  const auto mask = support(config_.kind, component, config_.gray);
  for (unsigned i = 0; i < 3; ++i)
    in[i] = Fast::point(
        (mask & (1u << i)) ? numeric_double(bits[i], config_.narrow) : 0);
  const auto point = [](double x) { return Fast::point(x); };
  const auto linear = [&](const std::array<Fast, 3>& row,
                          const std::array<Fast, 3>& values,
                          unsigned support_mask) {
    auto value = point(0);
    for (unsigned i = 0; i < 3; ++i)
      if (support_mask & (1u << i))
        value = value + row[i] * values[i];
    return value;
  };
  const auto cie = [&](Fast value) -> std::optional<Fast> {
    if (value.low > constants_.epsilon_fast.high)
      return fast_root(value);
    if (value.high <= constants_.epsilon_fast.low)
      return (point(841) * value) / point(108) + point(4) / point(29);
    return {};
  };
  const auto inverse_cie = [&](Fast value) -> std::optional<Fast> {
    if (value.low > constants_.delta_fast.high)
      return value * value * value;
    if (value.high <= constants_.delta_fast.low)
      return point(108) / point(841) * (value - point(4) / point(29));
    return {};
  };
  switch (config_.kind) {
    case Kind::RgbToYcbcr:
    case Kind::YcbcrToRgb:
      return linear(constants_.first_fast[component], in, mask);
    case Kind::GrayToColor:
      return in[0] * constants_.white_xz_fast[component == 0 ? 0 : 1];
    case Kind::XyzToLab: {
      auto y = cie(in[1]);
      if (!y)
        return {};
      if (component == 0)
        return (point(116) * *y - point(16)) / point(100);
      auto other = cie(in[component == 1 ? 0 : 2] /
                       constants_.white_xz_fast[component == 1 ? 0 : 1]);
      if (!other)
        return {};
      return component == 1 ? point(500) * (*other - *y)
                            : point(200) * (*y - *other);
    }
    case Kind::LabToXyz: {
      const auto fy = (point(100) * in[0] + point(16)) / point(116);
      auto t = component == 1   ? fy
               : component == 0 ? fy + in[1] / point(500)
                                : fy - in[2] / point(200);
      auto g = inverse_cie(t);
      if (!g)
        return {};
      return component == 1
                 ? *g
                 : *g * constants_.white_xz_fast[component == 0 ? 0 : 1];
    }
    case Kind::XyzToOklab: {
      std::array<Fast, 3> roots;
      for (unsigned i = 0; i < 3; ++i) {
        auto r = fast_root(linear(constants_.first_fast[i], in, 7));
        if (!r)
          return {};
        roots[i] = *r;
      }
      return linear(constants_.second_fast[component], roots, 7);
    }
    case Kind::OklabToXyz: {
      std::array<Fast, 3> cubes;
      for (unsigned i = 0; i < 3; ++i) {
        auto r = linear(constants_.first_fast[i], in, 7);
        cubes[i] = r * r * r;
      }
      return linear(constants_.second_fast[component], cubes, 7);
    }
    case Kind::XyzToXyy: {
      const auto total = in[0] + in[1] + in[2];
      return total.nonzero() ? std::optional<Fast>(in[component] / total)
                             : std::nullopt;
    }
    case Kind::XyyToXyz:
      if (!in[1].nonzero())
        return {};
      return ((component == 0 ? in[0] : point(1) - in[0] - in[1]) * in[2]) /
             in[1];
    case Kind::RgbToHsl:
    case Kind::RgbToHsv: {
      const auto e = extrema(bits, config_.narrow);
      const auto minimum = in[e[0]], maximum = in[e[1]];
      if (component == 2)
        return config_.kind == Kind::RgbToHsl ? (maximum + minimum) / point(2)
                                              : maximum;
      if (component == 1 && minimum.low != maximum.low) {
        const auto denominator =
            config_.kind == Kind::RgbToHsl
                ? point(1) - fast_abs(maximum + minimum - point(1))
                : maximum;
        if (denominator.nonzero())
          return (maximum - minimum) / denominator;
      }
      return {};
    }
    default:
      return {};
  }
}
std::optional<std::uint64_t> ModelMath::accept(
    Fast bound, std::optional<double> candidate) const {
  if (!bound.finite())
    return {};
  if (config_.profile != SequenceProfile::Strict)
    return bound.accepted(
        candidate.value_or(bound.low + (bound.high - bound.low) * .5),
        config_.narrow);
  // A binary64 directed enclosure which lies in one binary32 rounding cell
  // certifies the *complete* expression; it is not a rounded-intermediate path.
  if (config_.narrow) {
    const auto low = numeric_bits(bound.low, true);
    const auto high = numeric_bits(bound.high, true);
    if (low == high && BinaryParts::decode(low, true).magnitude &&
        !BinaryParts::decode(low, true).infinite)
      return low;
  }
  return {};
}
Result<std::uint64_t> ModelMath::evaluate(
    const std::array<std::uint64_t, 3>& bits, unsigned component,
    std::optional<double> candidate) {
  using Answer = Result<std::uint64_t>;
  try {
    if (component > 2)
      return Answer(
          Status{ErrorCode::Internal, "FMT-11 component outside arity"});
    exact_.work(1);
    const auto mask = support(config_.kind, component, config_.gray);
    bool all_finite = true;
    for (unsigned i = 0; i < 3; ++i) {
      if (!(mask & (1u << i)))
        continue;
      const auto value = BinaryParts::decode(bits[i], config_.narrow);
      all_finite = all_finite && !value.nan && !value.infinite;
      if (config_.semantic && (value.nan || value.infinite))
        return Answer(domain("FMT-11 semantic component must be finite"));
    }
    if (config_.kind == Kind::BinaryToGray) {
      const auto x = BinaryParts::decode(bits[0], config_.narrow);
      const auto one =
          config_.narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
      if (x.magnitude != 0 && bits[0] != one)
        return Answer(
            domain("BlackWhite selector must be exactly zero or one"));
      return Answer(config_.levels[x.magnitude ? 1 : 0]);
    }
    const int copy = copied_component(config_.kind, component, config_.gray);
    if (copy >= 0)
      return Answer(bits[static_cast<unsigned>(copy)]);
    if (copy == -2)
      return Answer(UINT64_C(0));
    if (config_.kind != Kind::Threshold) {
      for (unsigned i = 0; i < 3; ++i)
        if ((mask & (1u << i)) &&
            BinaryParts::decode(bits[i], config_.narrow).nan)
          return Answer(numeric_ops::converted_nan(
              bits[i],
              config_.narrow ? ElementType::Float32 : ElementType::Float64,
              config_.narrow ? ElementType::Float32 : ElementType::Float64));
    }
    if (config_.semantic && (config_.kind == Kind::LchToLab ||
                             config_.kind == Kind::OklchToOklab)) {
      const auto c = BinaryParts::decode(bits[1], config_.narrow);
      if (c.negative && c.magnitude)
        return Answer(domain("semantic polar chroma must be nonnegative"));
    }
    if (all_finite && config_.algorithm != Algorithm::Reference) {
      const auto bound = fast(bits, component);
      if (bound) {
        const auto accepted = accept(*bound, candidate);
        if (accepted) {
          ++counters_.accepted;
          return Answer(*accepted);
        }
      }
    }
    ++counters_.reference;
    const auto result = reference(bits, component);
    if (config_.semantic) {
      const auto classified = BinaryParts::decode(result, config_.narrow);
      if (classified.infinite || classified.nan)
        return Answer(Status{ErrorCode::OperationFailed,
                             "FMT-11 semantic result is not finite",
                             FailureReason::ArithmeticOverflow,
                             {FailureOrigin::Domain, FailureScope::Atom}});
    }
    return Answer(result);
  } catch (const Status& status) {
    return Answer(status);
  } catch (const numeric_ops::DirectedInterval::Unresolved&) {
    return Answer(
        Status{ErrorCode::ResourceExhausted,
               "FMT-11 could not certify rounding within bounded precision",
               FailureReason::CapacityLimit});
  }
}
}  // namespace ps::plugin_internal::model_ops
