#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "01-numeric/lowpass_geometry.hpp"
#include "01-numeric/lowpass_polynomial.hpp"

namespace ps::plugin_internal::numeric_ops {
struct NonuniformLowpassMath {
  CheckedLowpassKernel kernel;
  CheckedLowpassPolynomialWorkspace polynomial_workspace;
  using Math = CheckedDirectedInterval;
  using Number = Math::Number;
  using Interval = Math::Interval;
  using Frame = Math::Frame;
  // Persistent traversal storage is part of invocation scratch, not the stack.
  Number center, radius, lower, upper, cursor;
  CheckedLowpassPiece positive_piece, negative_piece;
  template <class Values>
  void fill(CheckedLowpassPiece& piece, const Values& values) {
    auto& m = kernel.functions.math;
    if (!m.work(1))
      return;
    if (piece.first != UINT64_MAX) {
      piece.first_value = values(piece.first);
      piece.last_value = values(piece.last);
    }
  }
  template <class Positions, class Values, class Visitor>
  void visit(const Positions& positions, const Values& values, unsigned count,
             const std::string& boundary, const Visitor& visitor) {
    auto& m = kernel.functions.math;
    CheckedLowpassGeometry geometry(m);
    m.copy(cursor, lower);
    while (m.active() && m.compare(cursor, upper) < 0) {
      geometry.piece_at(positive_piece, positions, count, cursor, lower, upper,
                        false, boundary);
      fill(positive_piece, values);
      if (!m.active())
        return;
      visitor(positive_piece);
      m.copy(cursor, positive_piece.upper);
    }
  }
  void integer_input(Number& out, std::uint64_t raw, bool narrow = false) {
    auto& m = kernel.functions.math;
    if (!m.work(Math::kWords))
      return;
    const auto value = BinaryParts::decode(raw, narrow);
    out.magnitude.set(value, 1074);
    out.negative = value.negative && value.magnitude;
  }
  void subtract(Number& out, const Number& a, const Number& b) {
    auto& m = kernel.functions.math;
    Frame frame(m);
    auto& opposite = m.number();
    m.copy(opposite, b);
    if (m.top(opposite) >= 0)
      opposite.negative = !opposite.negative;
    m.add(out, a, opposite);
  }
  // All operands below are integers (m.precision=0), retaining exact products.
  // F(u)=(N+S*u)/H in units 2^-1074, where u=(world-center)/R.
  void affine_integer(const CheckedLowpassPiece& piece, const Number& center,
                      const Number& radius, bool narrow, Number& numerator,
                      Number& slope, Number& denominator) {
    auto& m = kernel.functions.math;
    Frame frame(m);
    auto& a = m.number();
    auto& b = m.number();
    auto& delta = m.number();
    auto& offset = m.number();
    auto& term = m.number();
    integer_input(a, piece.first_value, narrow);
    integer_input(b, piece.last_value, narrow);
    if (piece.first == piece.last) {
      m.copy(numerator, a);
      m.clear(slope);
      m.integer(denominator, 1);
      return;
    }
    subtract(denominator, piece.end, piece.start);
    subtract(delta, b, a);
    subtract(offset, center, piece.start);
    m.multiply(numerator, a, denominator, true);
    m.multiply(term, offset, delta, true);
    m.add(numerator, numerator, term);
    m.multiply(slope, radius, delta, true);
  }
  template <class Positions, class Values>
  std::optional<std::uint64_t> exact_even(const Positions& positions,
                                          const Values& values, unsigned count,
                                          const std::string& boundary,
                                          bool narrow) {
    auto& m = kernel.functions.math;
    Frame frame(m);
    CheckedLowpassGeometry geometry(m);
    m.precision = 0;
    auto& distance = m.number();
    auto& right_cursor = m.number();
    auto& left_cursor = m.number();
    auto& next_right = m.number();
    auto& next_left = m.number();
    auto& an = m.number();
    auto& as = m.number();
    auto& ah = m.number();
    auto& bn = m.number();
    auto& bs = m.number();
    auto& bh = m.number();
    auto& left = m.number();
    auto& right = m.number();
    auto& sum = m.number();
    auto& denominator = m.number();
    auto& saved_n = m.number();
    auto& saved_d = m.number();
    bool first = true;
    while (m.active() && m.compare(distance, radius) < 0) {
      m.add(right_cursor, center, distance);
      subtract(left_cursor, center, distance);
      geometry.piece_at(positive_piece, positions, count, right_cursor, lower,
                        upper, false, boundary);
      geometry.piece_at(negative_piece, positions, count, left_cursor, lower,
                        upper, true, boundary);
      fill(positive_piece, values);
      fill(negative_piece, values);
      if (!m.active())
        return {};
      const auto& a = positive_piece;
      const auto& b = negative_piece;
      affine_integer(a, center, radius, narrow, an, as, ah);
      affine_integer(b, center, radius, narrow, bn, bs, bh);
      m.multiply(left, as, bh, true);
      m.multiply(right, bs, ah, true);
      if (m.compare(left, right) != 0)
        return {};
      m.multiply(left, an, bh, true);
      m.multiply(right, bn, ah, true);
      m.add(sum, left, right);
      m.multiply(denominator, ah, bh, true);
      if (first) {
        m.copy(saved_n, sum);
        m.copy(saved_d, denominator);
        first = false;
      } else {
        m.multiply(left, sum, saved_d, true);
        m.multiply(right, saved_n, denominator, true);
        if (m.compare(left, right) != 0)
          return {};
      }
      subtract(next_right, a.upper, center);
      subtract(next_left, center, b.lower);
      const auto comparison = m.compare(next_right, next_left);
      const auto& next = comparison < 0 ? next_right : next_left;
      if (!m.active())
        return {};
      if (m.compare(next, distance) <= 0) {
        m.fail(ErrorCode::Internal);
        return {};
      }
      m.copy(distance, next);
    }
    // Every admitted continuous kernel has positive full normalization:
    // Gaussian is positive; sinc windows are nonnegative/nonincreasing on
    // [0,R]. Integrating by parts against the strictly positive primitive
    // Si(a*t)/a proves positivity (pair successive sine lobes with 1/t).
    // If F(u)+F(-u)=C everywhere, the full quotient is exactly C/2.
    // N/S need <4204 bits, paired N/H <6306/4204, cross products <10510,
    // within 12288 bits. No rounding occurs in these identity comparisons.
    if (!m.active())
      return {};
    m.rounding.numerator = saved_n.magnitude;
    m.rounding.denominator = saved_d.magnitude;
    m.rounding.negative = saved_n.negative;
    auto rounded = m.rounding.round(narrow, *m.consume, -1075);
    if (!rounded.ok()) {
      m.fail(rounded.status().code, rounded.status().reason);
      return {};
    }
    return rounded.value();
  }
  // Coordinate scale cancels before division, so tiny widths cannot round to
  // zero as intermediate IEEE or Q_precision values.
  void coordinate_ratio(Interval out, const Number& numerator,
                        const Number& denominator) {
    auto& m = kernel.functions.math;
    m.divide(out.low, numerator, denominator, true);
    m.divide(out.high, numerator, denominator, false);
  }
  void widened(Interval output, Interval error) {
    auto& m = kernel.functions.math;
    Frame frame(m);
    auto& negative = m.number();
    m.copy(negative, error.high);
    if (m.top(negative) >= 0)
      negative.negative = true;
    m.add(output.low, output.low, negative);
    m.add(output.high, output.high, error.high);
  }
  template <class Positions, class Values>
  Result<std::uint64_t> evaluate(const Positions& positions,
                                 const Values& values, unsigned count,
                                 unsigned center_index,
                                 const std::string& boundary,
                                 const LowpassParameters& p, bool narrow,
                                 const core_internal::WorkConsumer& consume) {
    using Answer = Result<std::uint64_t>;
    auto& m = kernel.functions.math;
    m.reset();
    m.consume = consume;
    struct End {
      Math& m;
      ~End() {
        m.consume = nullptr;
        m.used = 0;
      }
    } end{m};
    integer_input(center, positions(center_index));
    integer_input(radius, p.radius);
    subtract(lower, center, radius);
    m.add(upper, center, radius);
    bool first = true, constant = true;
    std::uint64_t constant_bits = 0;
    int value_scale = -1074;
    visit(positions, values, count, boundary, [&](const auto& piece) {
      for (auto bits : {piece.first_value, piece.last_value}) {
        if (first) {
          constant_bits = bits;
          first = false;
        }
        constant = constant && bits == constant_bits;
        const auto v = BinaryParts::decode(bits, narrow);
        if (v.magnitude)
          value_scale = std::max(
              value_scale, v.exponent + 64 - __builtin_clzll(v.significand));
      }
    });
    if (m.fault == Math::Fault::Failure)
      return Answer(m.status());
    if (m.active() && constant)
      return Answer(constant_bits);
    auto exact = exact_even(positions, values, count, boundary, narrow);
    if (m.fault == Math::Fault::Failure)
      return Answer(m.status());
    if (m.active() && exact)
      return Answer(*exact);
    for (unsigned precision = 128; precision <= 4096; precision *= 2) {
      for (unsigned order = 16; order <= 512; order *= 2) {
        m.reset();
        m.precision = precision;
        {
          Frame frame(m);
          CheckedLowpassPolynomialBuilder builder(kernel, polynomial_workspace);
          auto polynomial = builder.build(p, order);
          if (m.fault == Math::Fault::Failure)
            return Answer(m.status());
          if (!m.active())
            continue;
          // More Taylor terms reduce truncation error; more Q precision
          // reduces coefficient/coordinate roundoff. Keep them separate.
          const bool small_tail = m.top(polynomial.error.high) <= 8;
          auto lo = m.interval(), hi = m.interval(), a = m.interval(),
               b = m.interval(), numerator = m.interval(),
               denominator = m.interval(), part = m.interval(),
               error = m.interval(), amplitude = m.interval(),
               length = m.interval(), temporary = m.interval(),
               source0 = m.interval(), source1 = m.interval(),
               delta = m.interval(), quotient = m.interval();
          auto& width = m.number();
          auto& offset = m.number();
          auto& difference = m.number();
          m.integer(lo, -1);
          m.integer(hi, 1);
          m.integer(a, 1);
          m.integer(b, 0);
          builder.integral(denominator, polynomial, lo, hi, a, b);
          m.scale(error, polynomial.error.view(), 1);
          widened(denominator, error);
          if (m.fault == Math::Fault::Failure)
            return Answer(m.status());
          if (!m.active())
            continue;
          if (denominator.low.negative || m.top(denominator.low) < 0) {
            if (small_tail)
              break;
            continue;
          }
          m.integer(numerator, 0);
          visit(positions, values, count, boundary, [&](const auto& piece) {
            subtract(difference, piece.lower, center);
            coordinate_ratio(lo, difference, radius);
            subtract(difference, piece.upper, center);
            coordinate_ratio(hi, difference, radius);
            const auto v0 = BinaryParts::decode(piece.first_value, narrow),
                       v1 = BinaryParts::decode(piece.last_value, narrow);
            m.dyadic(source0, v0.significand, v0.exponent - value_scale,
                     v0.negative);
            m.dyadic(source1, v1.significand, v1.exponent - value_scale,
                     v1.negative);
            m.copy(a, source0);
            m.integer(b, 0);
            if (piece.first != piece.last) {
              subtract(width, piece.end, piece.start);
              subtract(offset, center, piece.start);
              m.subtract(delta, source1, source0);
              coordinate_ratio(temporary, offset, width);
              m.multiply(temporary, temporary, delta);
              m.add(a, a, temporary);
              coordinate_ratio(b, radius, width);
              m.multiply(b, b, delta);
            }
            builder.integral(part, polynomial, lo, hi, a, b);
            m.integer(amplitude, 0);
            for (auto* value :
                 {&source0.low, &source0.high, &source1.low, &source1.high})
              if (m.compare_unsigned(value->magnitude,
                                     amplitude.high.magnitude) > 0) {
                m.copy(amplitude.high, *value);
                amplitude.high.negative = false;
              }
            subtract(difference, piece.upper, piece.lower);
            coordinate_ratio(length, difference, radius);
            m.multiply(error, length, amplitude);
            m.multiply(error, error, polynomial.error.view());
            widened(part, error);
            m.add(numerator, numerator, part);
          });
          m.divide(quotient, numerator, denominator);
          const auto rounded = [&](const Number& value) {
            if (!m.work(2 * Math::kWords))
              return UINT64_C(0);
            m.rounding.numerator = value.magnitude;
            m.rounding.denominator.words.fill(0);
            m.rounding.denominator.words[0] = 1;
            m.rounding.negative = value.negative;
            auto result = m.rounding.round(
                narrow, consume, value_scale - static_cast<int>(precision));
            if (!result.ok()) {
              m.fail(result.status().code, result.status().reason);
              return UINT64_C(0);
            }
            return result.value();
          };
          const auto low = rounded(quotient.low), high = rounded(quotient.high);
          if (m.fault == Math::Fault::Failure)
            return Answer(m.status());
          if (m.active() && low == high)
            return Answer(low);
          if (small_tail)
            break;
        }
      }
    }
    return Answer(
        Status{ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
  }
};
}  // namespace ps::plugin_internal::numeric_ops
