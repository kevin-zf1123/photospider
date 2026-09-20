#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "01-numeric/directed_color_functions.hpp"
#include "01-numeric/exact_polynomial.hpp"
#include "photospider/data/color_array.hpp"

namespace ps::plugin_internal::numeric_ops {
// Algebraic RGB paths: direct association conversion, linear transfer and
// gamma=2. All source floats denote exact dyadic values; implicit alpha is 1.
// B=2^-1074. Products below need fewer than 13000 bits including square-root
// midpoint alignment, within the shared 40960-bit polynomial integer arena.
class ExactRgb final {
  ExactPolynomial exact_;
  DirectedColorFunctions directed_;
  using Index = ExactPolynomial::Index;
  using Interval = DirectedInterval::Interval;
  using Frame = DirectedInterval::Frame;
  void check() const {
    if (!exact_.status().ok())
      throw exact_.status();
  }
  void import(Interval output, Index value) {
    check();
    auto& math = directed_.functions.math;
    const auto& words = exact_.number(value).magnitude.words;
    math.work(ExactPolynomial::kWords + DirectedInterval::kWords);
    for (unsigned j = DirectedInterval::kWords; j < words.size(); ++j)
      if (words[j])
        DirectedInterval::capacity();
    std::copy_n(words.begin(), DirectedInterval::kWords,
                output.low.magnitude.words.begin());
    output.low.negative = false;
    math.copy(output.high, output.low);
  }
  void ratio(Interval output, Index n, Index d, int scale = 0) {
    auto& math = directed_.functions.math;
    Frame frame(math);
    auto numerator = math.interval(), denominator = math.interval();
    import(numerator, n);
    import(denominator, d);
    math.divide(output, numerator, denominator);
    if (scale)
      math.scale(output, output, scale);
  }
  int normalized_ratio(Interval output, Index n, Index d) {
    auto& math = directed_.functions.math;
    Frame frame(math);
    auto numerator = math.interval(), denominator = math.interval();
    import(numerator, n);
    import(denominator, d);
    const auto ntop = math.top(numerator.low), dtop = math.top(denominator.low);
    math.scale(numerator, numerator, static_cast<int>(math.precision) - ntop);
    math.scale(denominator, denominator,
               static_cast<int>(math.precision) - dtop);
    math.divide(output, numerator, denominator);
    return ntop - dtop;
  }
  std::uint64_t round_scaled(const DirectedInterval::Number& value, bool narrow,
                             int scale) {
    auto& math = directed_.functions.math;
    math.work(2 * DirectedInterval::kWords);
    math.rounding.numerator = value.magnitude;
    math.rounding.denominator.words.fill(0);
    math.rounding.denominator.words[0] = 1;
    math.rounding.negative = value.negative;
    auto result = math.rounding.round(narrow, *math.consume,
                                      scale - static_cast<int>(math.precision));
    if (!result.ok())
      throw result.status();
    return result.value();
  }
  void constant_ratio(Interval output, unsigned n, unsigned d) {
    auto& math = directed_.functions.math;
    math.integer(output, n);
    math.divide_small(output, output, d);
  }
  void srgb_decode(Interval output, Index numerator, Index denominator,
                   bool low_segment) {
    auto& math = directed_.functions.math;
    Frame frame(math);
    auto base = math.interval(), exponent = math.interval();
    ratio(base, numerator, denominator);
    if (low_segment) {
      math.integer(exponent, 25);
      math.multiply(output, base, exponent);
      math.divide_small(output, output, 323);
      return;
    }
    auto offset = math.interval();
    constant_ratio(offset, 11, 200);
    math.add(base, base, offset);
    math.integer(offset, 200);
    math.multiply(base, base, offset);
    math.divide_small(base, base, 211);
    constant_ratio(exponent, 12, 5);
    directed_.power(output, base, exponent);
  }
  void srgb_encode_high(Interval output, Interval input) {
    auto& math = directed_.functions.math;
    Frame frame(math);
    auto exponent = math.interval(), offset = math.interval();
    constant_ratio(exponent, 5, 12);
    directed_.power(output, input, exponent);
    math.integer(offset, 211);
    math.multiply(output, output, offset);
    math.divide_small(output, output, 200);
    constant_ratio(offset, 11, 200);
    math.subtract(output, output, offset);
  }
  void srgb_encode(Interval output, Interval input) {
    auto& math = directed_.functions.math;
    Frame frame(math);
    auto join = math.interval(), factor = math.interval();
    constant_ratio(join, 7827, 2500000);
    constant_ratio(factor, 323, 25);
    if (math.compare(input.high, join.low) <= 0) {
      math.multiply(output, input, factor);
      return;
    }
    if (math.compare(input.low, join.high) > 0) {
      srgb_encode_high(output, input);
      return;
    }
    // The two standard decimal thresholds leave a downward encoding jump.
    // Across the uncertain join, enclose each monotone branch separately.
    auto low_domain = math.interval(), high_domain = math.interval(),
         low_result = math.interval(), high_result = math.interval();
    math.copy(low_domain, input);
    math.copy(high_domain, input);
    if (math.compare(low_domain.high, join.high) > 0)
      math.copy(low_domain.high, join.high);
    if (math.compare(high_domain.low, join.low) < 0)
      math.copy(high_domain.low, join.low);
    math.multiply(low_result, low_domain, factor);
    srgb_encode_high(high_result, high_domain);
    math.copy(output.low, math.compare(low_result.low, high_result.low) < 0
                              ? low_result.low
                              : high_result.low);
    math.copy(output.high, math.compare(low_result.high, high_result.high) > 0
                               ? low_result.high
                               : high_result.high);
  }
  Result<std::uint64_t> rounded(Index n, Index d, bool narrow, int scale,
                                bool negative_zero = false) {
    if (!exact_.status().ok())
      return Result<std::uint64_t>(exact_.status());
    if (!exact_.sign(n) && negative_zero)
      return Result<std::uint64_t>(UINT64_C(1) << (narrow ? 31 : 63));
    return exact_.round(n, d, narrow, scale);
  }
  Index signed_square(Index value) {
    auto squared = exact_.multiply(value, value);
    if (exact_.sign(value) < 0)
      squared = exact_.add(exact_.integer(0), squared, true);
    return squared;
  }
  int compare_products(Index a, Index b, Index c, Index d) {
    const auto saved = exact_.mark();
    const auto left = exact_.multiply(a, b), right = exact_.multiply(c, d);
    check();
    const auto order = exact_.compare_magnitude(left, right);
    exact_.restore(saved);
    return order;
  }
  int compare_scaled(Index a, unsigned b, Index c, unsigned d) {
    const auto saved = exact_.mark();
    const auto order =
        compare_products(a, exact_.integer(b), c, exact_.integer(d));
    exact_.restore(saved);
    return order;
  }
  Index integer_power(Index value, unsigned exponent) {
    auto result = exact_.integer(1);
    while (exponent) {
      if (exponent & 1)
        result = exact_.multiply(result, value);
      exponent >>= 1;
      if (exponent)
        value = exact_.multiply(value, value);
    }
    return result;
  }

  Result<std::uint64_t> nonlinear_component(
      const std::array<Index, 2>& c, const std::array<Index, 2>& denominator,
      const std::array<Index, 2>& beta, Index weighted_alpha, Index h,
      bool premultiplied, bool srgb, std::uint64_t gamma, bool narrow) {
    auto& math = directed_.functions.math;
    const auto zero = exact_.integer(0);
    std::array<Index, 2> magnitude{};
    std::array<bool, 2> low{};
    for (unsigned i = 0; i < 2; ++i) {
      magnitude[i] =
          exact_.sign(c[i]) < 0 ? exact_.add(zero, c[i], true) : c[i];
      low[i] = compare_scaled(magnitude[i], 20000, denominator[i], 809) <= 0;
    }
    const auto order = compare_products(magnitude[0], denominator[1],
                                        magnitude[1], denominator[0]);
    const bool same = order == 0 && exact_.sign(c[0]) == exact_.sign(c[1]);
    const bool opposite = order == 0 && exact_.sign(c[0]) == -exact_.sign(c[1]);
    if (opposite && exact_.compare_magnitude(beta[0], beta[1]) == 0)
      return Result<std::uint64_t>(0);
    if (!srgb && exact_.sign(c[0]) == -exact_.sign(c[1])) {
      // Frequent exact cancellations under small integer gamma have an exact
      // cross-product proof. An enclosure alone cannot establish equality.
      // Each source ratio cross-product has <3175 bits; eighth powers with
      // beta remain below 28600 bits in the 40960-bit admitted integer arena.
      constexpr std::array<std::uint64_t, 6> integer_gamma{
          UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000),
          UINT64_C(0x4014000000000000), UINT64_C(0x4018000000000000),
          UINT64_C(0x401c000000000000), UINT64_C(0x4020000000000000)};
      for (unsigned i = 0; i < integer_gamma.size(); ++i) {
        if (gamma != integer_gamma[i])
          continue;
        const auto saved = exact_.mark();
        const auto left = exact_.multiply(
            beta[0], integer_power(
                         exact_.multiply(magnitude[0], denominator[1]), i + 3));
        const auto right = exact_.multiply(
            beta[1], integer_power(
                         exact_.multiply(magnitude[1], denominator[0]), i + 3));
        check();
        const bool equal = exact_.compare_magnitude(left, right) == 0;
        exact_.restore(saved);
        if (equal)
          return Result<std::uint64_t>(0);
        break;
      }
    }
    const bool active0 = exact_.sign(beta[0]) && exact_.sign(c[0]);
    const bool active1 = exact_.sign(beta[1]) && exact_.sign(c[1]);
    if (!active0 && !active1)
      return Result<std::uint64_t>(0);
    // A single nonzero contribution is not generally a transfer round trip:
    // its weight can be below one. Cancellation requires the other alpha to
    // vanish, or both encoded straight inputs to be mathematically identical.
    int selected = !exact_.sign(beta[1]) ? 0 : !exact_.sign(beta[0]) ? 1 : -1;
    if (same)
      selected = 0;
    const bool inverse =
        selected >= 0 && (!srgb || !low[selected] ||
                          compare_scaled(magnitude[selected], 62500000,
                                         denominator[selected], 2528121) <= 0);
    Index rational_n = zero, rational_d = zero;
    bool rational = inverse;
    if (inverse) {
      rational_n = c[selected];
      rational_d = denominator[selected];
    } else if (srgb && low[0] && low[1]) {
      rational_n = exact_.add(
          exact_.multiply(exact_.multiply(beta[0], c[0]), denominator[1]),
          exact_.multiply(exact_.multiply(beta[1], c[1]), denominator[0]));
      rational_d = exact_.multiply(
          exact_.multiply(weighted_alpha, denominator[0]), denominator[1]);
      rational = compare_scaled(rational_n, 62500000, rational_d, 2528121) <= 0;
    }
    if (rational) {
      int scale = 0;
      if (premultiplied) {
        rational_n = exact_.multiply(rational_n, weighted_alpha);
        rational_d = exact_.multiply(rational_d, h);
        scale = -1074;
      }
      return rounded(rational_n, rational_d, narrow, scale);
    }
    check();
    const auto maximum = order >= 0 ? 0u : 1u;
    std::array<Index, 2> normalized_n{}, normalized_d{};
    if (!srgb) {
      for (unsigned i = 0; i < 2; ++i) {
        normalized_n[i] = exact_.multiply(magnitude[i], denominator[maximum]);
        normalized_d[i] = exact_.multiply(denominator[i], magnitude[maximum]);
      }
    }
    check();
    for (unsigned precision = 128; precision <= 4096; precision *= 2) {
      math.work(1);
      math.precision = precision;
      Frame frame(math);
      try {
        auto contribution0 = math.interval(), contribution1 = math.interval(),
             weight = math.interval(), exponent = math.interval(),
             base = math.interval(), mixed = math.interval(),
             encoded = math.interval(), one = math.interval();
        math.integer(one, 1);
        if (!srgb)
          math.raw(exponent, gamma, false);
        for (unsigned i = 0; i < 2; ++i) {
          auto contribution = i ? contribution1 : contribution0;
          if (!exact_.sign(beta[i]) || !exact_.sign(c[i])) {
            math.integer(contribution, 0);
            continue;
          }
          if (srgb) {
            srgb_decode(contribution, magnitude[i], denominator[i], low[i]);
          } else {
            ratio(base, normalized_n[i], normalized_d[i]);
            // Exact normalized source magnitudes are in [0,1].
            if (math.compare(base.high, one.high) > 0)
              math.copy(base.high, one.high);
            directed_.power(contribution, base, exponent);
          }
          ratio(weight, beta[i], weighted_alpha);
          math.multiply(contribution, contribution, weight);
          if (exact_.sign(c[i]) < 0)
            math.negate(contribution, contribution);
        }
        math.add(mixed, contribution0, contribution1);
        if (math.includes_zero(mixed))
          throw DirectedInterval::Unresolved{};
        const bool negative = mixed.high.negative;
        if (negative)
          math.negate(mixed, mixed);
        int output_scale = 0;
        if (srgb) {
          srgb_encode(encoded, mixed);
        } else {
          // Homogeneity removes huge intermediate powers: m*R^(1/gamma),
          // with |R|<=1. The reciprocal remains a directed exact-real ratio.
          if (math.compare(mixed.high, one.high) > 0)
            math.copy(mixed.high, one.high);
          math.divide(exponent, one, exponent);
          directed_.power(encoded, mixed, exponent);
          output_scale =
              normalized_ratio(base, magnitude[maximum], denominator[maximum]);
          math.multiply(encoded, encoded, base);
        }
        if (premultiplied) {
          // Normalize the exact alpha ratio before multiplying, and apply
          // its power of two last. A tiny alpha must not lose all relative
          // precision before multiplying an HDR straight encoded value.
          const auto exponent = normalized_ratio(weight, weighted_alpha, h);
          math.multiply(encoded, encoded, weight);
          output_scale += exponent - 1074;
        }
        if (encoded.low.negative)
          math.clear(encoded.low);
        const auto lower = round_scaled(encoded.low, narrow, output_scale);
        const auto upper = round_scaled(encoded.high, narrow, output_scale);
        if (lower == upper)
          return Result<std::uint64_t>(
              lower | (negative ? UINT64_C(1) << (narrow ? 31 : 63) : 0));
      } catch (const DirectedInterval::Unresolved&) {
        // Refine the entire expression; no rounded intermediate survives.
      }
    }
    return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted,
                                        "RGB transfer rounding precision limit",
                                        FailureReason::CapacityLimit});
  }

 public:
  explicit ExactRgb(SequenceProfile profile)
      : exact_(profile), directed_(profile) {}
  Result<std::array<std::uint64_t, 4>> nonlinear(
      const std::array<std::uint64_t, 2>& stops, std::uint64_t query,
      const std::array<std::array<std::uint64_t, 4>, 2>& rows,
      ColorAssociation input, ColorAssociation output, bool srgb,
      std::uint64_t gamma, bool narrow,
      const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::array<std::uint64_t, 4>>;
    exact_.begin(consume);
    auto& math = directed_.functions.math;
    math.used = 0;
    math.consume = &consume;
    struct End {
      ExactPolynomial& exact;
      DirectedInterval& math;
      ~End() {
        exact.end();
        math.consume = nullptr;
      }
    } end{exact_, math};
    try {
      constexpr auto one_bits = UINT64_C(0x3ff0000000000000);
      const auto one = exact_.integer(1), unit = exact_.shift(one, 1074);
      const auto s0 = exact_.binary(stops[0], 1074),
                 s1 = exact_.binary(stops[1], 1074);
      const auto x = exact_.binary(query, 1074);
      const auto u = exact_.add(s1, x, true), v = exact_.add(x, s0, true);
      const auto h = exact_.add(u, v);
      std::array<Index, 2> alpha{}, beta{};
      for (unsigned i = 0; i < 2; ++i) {
        alpha[i] = exact_.binary(
            input == ColorAssociation::None ? one_bits : rows[i][3], 1074);
        beta[i] = exact_.multiply(i ? v : u, alpha[i]);
      }
      const auto weighted_alpha = exact_.add(beta[0], beta[1]);
      check();
      std::array<std::uint64_t, 4> result{};
      if (!exact_.sign(weighted_alpha))
        return Answer(result);
      auto a = rounded(weighted_alpha, h, narrow, -1074);
      if (!a.ok())
        return Answer(a.status());
      result[3] = a.value();
      const auto saved = exact_.mark();
      for (unsigned channel = 0; channel < 3; ++channel) {
        exact_.restore(saved);
        std::array<Index, 2> c{}, denominator{};
        for (unsigned i = 0; i < 2; ++i) {
          c[i] = exact_.sign(alpha[i]) ? exact_.binary(rows[i][channel], 1074)
                                       : exact_.integer(0);
          denominator[i] =
              input == ColorAssociation::Premultiplied && exact_.sign(alpha[i])
                  ? alpha[i]
                  : unit;
        }
        auto value = nonlinear_component(
            c, denominator, beta, weighted_alpha, h,
            output == ColorAssociation::Premultiplied, srgb, gamma, narrow);
        if (!value.ok())
          return Answer(value.status());
        if (BinaryParts::decode(value.value(), narrow).infinite)
          return Answer(Status{ErrorCode::OperationFailed,
                               "RGB output overflow",
                               FailureReason::ArithmeticOverflow});
        result[channel] = value.value();
      }
      if (!BinaryParts::decode(result[3], narrow).magnitude) {
        for (unsigned channel = 0; channel < 3; ++channel)
          if (BinaryParts::decode(result[channel], narrow).magnitude)
            return Answer(
                Status{ErrorCode::OperationFailed,
                       "positive alpha rounds to zero with nonzero RGB",
                       FailureReason::AssociationUnderflow});
        result.fill(0);
      }
      return Answer(result);
    } catch (const Status& status) {
      return Answer(status);
    }
  }
  // Caller validates complete rows and association, and selects this entry only
  // for direct, linear/gamma=1, or gamma=2. A four-word result has alpha at [3]
  // even for RGB; the caller publishes only its declared three/four channels.
  Result<std::array<std::uint64_t, 4>> algebraic(
      const std::array<std::uint64_t, 2>& stops, std::uint64_t query,
      const std::array<std::array<std::uint64_t, 4>, 2>& rows,
      ColorAssociation input, ColorAssociation output, bool direct, bool square,
      bool narrow, const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::array<std::uint64_t, 4>>;
    exact_.begin(consume);
    struct End {
      ExactPolynomial& math;
      ~End() { math.end(); }
    } end{exact_};
    constexpr auto one_bits = UINT64_C(0x3ff0000000000000);
    const bool alpha = input != ColorAssociation::None;
    const bool premultiplied = input == ColorAssociation::Premultiplied;
    const bool output_premultiplied = output == ColorAssociation::Premultiplied;
    const auto one = exact_.integer(1);
    const auto a0 = exact_.binary(alpha ? rows[0][3] : one_bits, 1074);
    const auto a1 = exact_.binary(alpha ? rows[1][3] : one_bits, 1074);
    Index u = 0, v = 0, h = one, weighted_alpha = a0;
    if (!direct) {
      const auto s0 = exact_.binary(stops[0], 1074),
                 s1 = exact_.binary(stops[1], 1074);
      const auto x = exact_.binary(query, 1074);
      u = exact_.add(s1, x, true);
      v = exact_.add(x, s0, true);
      h = exact_.add(u, v);
      weighted_alpha =
          exact_.add(exact_.multiply(u, a0), exact_.multiply(v, a1));
    }
    if (!exact_.status().ok())
      return Answer(exact_.status());
    std::array<std::uint64_t, 4> result{};
    if (!exact_.sign(weighted_alpha))
      return Answer(result);
    auto alpha_bits = rounded(weighted_alpha, h, narrow, -1074);
    if (!alpha_bits.ok())
      return Answer(alpha_bits.status());
    result[3] = alpha_bits.value();
    const auto base = exact_.mark();
    for (unsigned channel = 0; channel < 3; ++channel) {
      exact_.restore(base);
      const auto c0 = exact_.binary(rows[0][channel], 1074);
      const auto c1 = exact_.binary(rows[1][channel], 1074);
      Index n = c0, d = one;
      int scale = -1074;
      bool root = false;
      if (direct) {
        if (!premultiplied && output_premultiplied) {
          n = exact_.multiply(c0, a0);
          scale = -2148;
        } else if (premultiplied && !output_premultiplied) {
          d = a0;
          scale = 0;
        }
      } else if (!square) {
        if (premultiplied)
          n = exact_.add(exact_.multiply(u, c0), exact_.multiply(v, c1));
        else
          n = exact_.add(exact_.multiply(exact_.multiply(u, a0), c0),
                         exact_.multiply(exact_.multiply(v, a1), c1));
        d = output_premultiplied ? h : weighted_alpha;
        scale = premultiplied ? (output_premultiplied ? -1074 : 0)
                              : (output_premultiplied ? -2148 : -1074);
      } else {
        root = true;
        const auto square0 = signed_square(c0), square1 = signed_square(c1);
        if (premultiplied) {
          // Zero-alpha premultiplied sources are validated black. Replacing
          // their otherwise-zero denominator by 1 preserves their zero term.
          const auto denominator0 = exact_.sign(a0) ? a0 : one;
          const auto denominator1 = exact_.sign(a1) ? a1 : one;
          n = exact_.add(
              exact_.multiply(exact_.multiply(u, square0), denominator1),
              exact_.multiply(exact_.multiply(v, square1), denominator0));
          const auto denominators = exact_.multiply(denominator0, denominator1);
          if (output_premultiplied) {
            n = exact_.multiply(n, weighted_alpha);
            d = exact_.multiply(exact_.multiply(h, h), denominators);
            scale = -2148;
          } else {
            d = exact_.multiply(denominators, weighted_alpha);
            scale = 0;
          }
        } else {
          n = exact_.add(exact_.multiply(exact_.multiply(u, a0), square0),
                         exact_.multiply(exact_.multiply(v, a1), square1));
          if (output_premultiplied) {
            n = exact_.multiply(n, weighted_alpha);
            d = exact_.multiply(h, h);
            scale = -4296;
          } else {
            d = weighted_alpha;
            scale = -2148;
          }
        }
      }
      auto value =
          root ? exact_.round_signed_sqrt_ratio(n, d, narrow, scale)
               : rounded(n, d, narrow, scale,
                         direct && rows[0][channel] == (UINT64_C(1) << 63));
      if (!value.ok())
        return Answer(value.status());
      if (BinaryParts::decode(value.value(), narrow).infinite)
        return Answer(Status{ErrorCode::OperationFailed, "RGB output overflow",
                             FailureReason::ArithmeticOverflow});
      result[channel] = value.value();
    }
    if (!BinaryParts::decode(result[3], narrow).magnitude) {
      for (unsigned channel = 0; channel < 3; ++channel)
        if (BinaryParts::decode(result[channel], narrow).magnitude)
          return Answer(Status{ErrorCode::OperationFailed,
                               "positive alpha rounds to zero with nonzero RGB",
                               FailureReason::AssociationUnderflow});
      result.fill(0);
    }
    return Answer(result);
  }
};
}  // namespace ps::plugin_internal::numeric_ops
