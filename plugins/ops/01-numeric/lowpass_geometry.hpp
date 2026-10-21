#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "01-numeric/directed_interval.hpp"

namespace ps::plugin_internal::numeric_ops {
// Exact world coordinates in units 2^-1074, never IEEE-rounded boundary math.
// Fields fit within 2102 bits for finite positions/center/radius; using the
// fixed Number stores every limb in invocation scratch.
struct CheckedLowpassPiece {
  using Math = CheckedDirectedInterval;
  typename Math::Number lower, upper, start, end;
  std::uint64_t first = UINT64_MAX, last = UINT64_MAX;
  std::uint64_t first_value = 0, last_value = 0;
};
class CheckedLowpassGeometry {
  using Math = CheckedDirectedInterval;
  using Number = typename Math::Number;
  using Frame = typename Math::Frame;
  using Piece = CheckedLowpassPiece;
  Math& m_;
  void subtract(Number& out, const Number& a, const Number& b) {
    Frame frame(m_);
    auto& negative = m_.number();
    m_.copy(negative, b);
    if (m_.top(negative) >= 0)
      negative.negative = !negative.negative;
    m_.add(out, a, negative);
  }
  // Integer quotient has no fixed-point scale. Denominator is strictly
  // positive.
  void floor_ratio(Number& out, const Number& a, const Number& b) {
    Frame frame(m_);
    auto& remainder = m_.number();
    m_.divide_unsigned(out.magnitude, remainder.magnitude, a.magnitude,
                       b.magnitude);
    out.negative = a.negative;
    if (a.negative && m_.top(remainder) >= 0)
      m_.increment(out.magnitude);
    m_.normalize(out);
  }
  void integer_product(Number& out, const Number& integer,
                       const Number& coordinate) {
    Frame frame(m_);
    auto& result = m_.number();
    const auto status = multiply_fixed(integer.magnitude, coordinate.magnitude,
                                       &result.magnitude, *m_.consume);
    if (!status.ok()) {
      m_.fail(status.code, status.reason);
      return;
    }
    result.negative = integer.negative != coordinate.negative;
    m_.normalize(result);
    m_.copy(out, result);
  }
  void raw(Number& out, std::uint64_t bits) {
    Frame frame(m_);
    auto value = m_.interval();
    m_.raw(value, bits, false);
    m_.copy(out, value.low);
  }
  template <class Positions>
  unsigned segment(const Positions& positions, unsigned count, const Number& at,
                   bool backward) {
    Frame frame(m_);
    auto& knot = m_.number();
    unsigned lo = 0, hi = count;
    while (lo < hi && m_.active()) {
      auto middle = lo + (hi - lo) / 2;
      raw(knot, positions(middle));
      const int order = m_.compare(knot, at);
      if (order < 0 || (!backward && !order))
        lo = middle + 1;
      else
        hi = middle;
    }
    return std::min(count - 2, lo ? lo - 1 : 0);
  }

 public:
  explicit CheckedLowpassGeometry(Math& math) : m_(math) {}
  // Return the affine world segment immediately to the requested side of
  // cursor, clipped to [lower, upper]. All coordinates are integers in units
  // 2^-1074. Positions is an allocation-free Float64 raw-word accessor;
  // positions must be finite and strictly increasing, count>=2. The caller
  // advances to piece.upper (forward) or piece.lower (backward).
  template <class Positions>
  void piece_at(Piece& piece, const Positions& positions, unsigned count,
                const Number& cursor, const Number& lower, const Number& upper,
                bool backward, const std::string& boundary) {
    const auto precision = m_.precision;
    m_.precision = 1074;
    struct Restore {
      Math& math;
      unsigned precision;
      ~Restore() { math.precision = precision; }
    } restore{m_, precision};
    if (!m_.work(8192))
      return;
    Frame frame(m_);
    auto& a = m_.number();
    auto& b = m_.number();
    auto& width = m_.number();
    auto& period = m_.number();
    auto& quotient = m_.number();
    auto& shifted = m_.number();
    auto& remainder = m_.number();
    auto& folded = m_.number();
    auto& first = m_.number();
    auto& last = m_.number();
    auto& temporary = m_.number();
    auto& twice_b = m_.number();
    raw(a, positions(0));
    raw(b, positions(count - 1));
    subtract(width, b, a);
    m_.copy(period, width);
    if (boundary == "reflect")
      m_.add(period, width, width);
    m_.add(twice_b, b, b);
    std::uint64_t i = UINT64_MAX, j = UINT64_MAX;
    const auto at_a = m_.compare(cursor, a), at_b = m_.compare(cursor, b);
    const bool exterior = boundary == "zero" || boundary == "replicate";
    if (exterior && (at_a < 0 || (backward && at_a == 0))) {
      m_.copy(piece.start, lower);
      m_.copy(piece.end, a);
      if (boundary == "replicate")
        i = j = 0;
    } else if (exterior && (at_b > 0 || (!backward && at_b == 0))) {
      m_.copy(piece.start, b);
      m_.copy(piece.end, upper);
      if (boundary == "replicate")
        i = j = count - 1;
    } else {
      bool reversed = false;
      m_.clear(shifted);
      m_.copy(folded, cursor);
      if (boundary == "reflect" || boundary == "wrap") {
        subtract(temporary, cursor, a);
        floor_ratio(quotient, temporary, period);
        integer_product(shifted, quotient, period);
        subtract(remainder, temporary, shifted);
        // A reverse traversal at a period seam owns the previous period.
        // Change the exact shift/remainder directly, avoiding a rounded probe.
        if (backward && m_.top(remainder) < 0) {
          subtract(shifted, shifted, period);
          m_.copy(remainder, period);
        }
        const auto at_turn = m_.compare(remainder, width);
        reversed = boundary == "reflect" &&
                   (at_turn > 0 || (at_turn == 0 && !backward));
        if (reversed) {
          subtract(temporary, cursor, shifted);
          subtract(folded, twice_b, temporary);
        } else {
          m_.add(folded, a, remainder);
        }
      }
      const auto selected =
          segment(positions, count, folded, backward != reversed);
      raw(first, positions(selected));
      raw(last, positions(selected + 1));
      if (reversed) {
        subtract(temporary, twice_b, last);
        m_.add(piece.start, temporary, shifted);
        subtract(temporary, twice_b, first);
        m_.add(piece.end, temporary, shifted);
        i = selected + 1;
        j = selected;
      } else {
        m_.add(piece.start, first, shifted);
        m_.add(piece.end, last, shifted);
        i = selected;
        j = selected + 1;
      }
    }
    if (!m_.active())
      return;
    m_.copy(piece.lower,
            m_.compare(piece.start, lower) > 0 ? piece.start : lower);
    m_.copy(piece.upper, m_.compare(piece.end, upper) < 0 ? piece.end : upper);
    // Isolated endpoint contacts never become integration pieces.
    if ((backward && m_.compare(piece.lower, cursor) >= 0) ||
        (!backward && m_.compare(piece.upper, cursor) <= 0)) {
      m_.fail(ErrorCode::Internal);
      return;
    }
    piece.first = i;
    piece.last = j;
    piece.first_value = piece.last_value = 0;
  }
};
}  // namespace ps::plugin_internal::numeric_ops
