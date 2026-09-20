#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "01-numeric/directed_interval.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps::plugin_internal::numeric_ops {
// Exact world coordinates in units 2^-1074, never IEEE-rounded boundary math.
// Fields fit within 2102 bits for finite positions/center/radius; using the
// common fixed Number admits every stored limb through the piece vector.
struct LowpassPiece {
  DirectedInterval::Number lower, upper, start, end;
  std::uint64_t first = UINT64_MAX, last = UINT64_MAX;
  std::uint64_t first_value = 0, last_value = 0;
};
class LowpassGeometry {
  using Number = DirectedInterval::Number;
  using Frame = DirectedInterval::Frame;
  DirectedInterval& m_;
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
    if (!status.ok())
      throw status;
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
  unsigned segment(const std::uint64_t* positions, unsigned count,
                   const Number& at, bool backward) {
    Frame frame(m_);
    auto& knot = m_.number();
    unsigned lo = 0, hi = count;
    while (lo < hi) {
      auto middle = lo + (hi - lo) / 2;
      raw(knot, positions[middle]);
      const int order = m_.compare(knot, at);
      if (order < 0 || (!backward && !order))
        lo = middle + 1;
      else
        hi = middle;
    }
    return std::min(count - 2, lo ? lo - 1 : 0);
  }

 public:
  explicit LowpassGeometry(DirectedInterval& math) : m_(math) {}
  // Appends pieces. Caller guarantees finite strictly increasing positions,
  // count>=2, center<count, finite positive radius, a valid boundary and a
  // synchronous work callback. Discard an incomplete appended range on failure.
  void partition(ResourceVector<LowpassPiece>* pieces,
                 const std::uint64_t* positions, unsigned count,
                 unsigned center, std::uint64_t radius,
                 const std::string& boundary) {
    if (m_.precision != 1074)
      DirectedInterval::capacity();
    Frame frame(m_);
    auto& a = m_.number();
    auto& b = m_.number();
    auto& middle = m_.number();
    auto& r = m_.number();
    auto& cursor = m_.number();
    auto& limit = m_.number();
    auto& width = m_.number();
    auto& period = m_.number();
    auto& quotient = m_.number();
    auto& shifted = m_.number();
    auto& remainder = m_.number();
    auto& folded = m_.number();
    auto& first = m_.number();
    auto& last = m_.number();
    auto& start = m_.number();
    auto& end = m_.number();
    auto& temporary = m_.number();
    auto& twice_b = m_.number();
    raw(a, positions[0]);
    raw(b, positions[count - 1]);
    raw(middle, positions[center]);
    raw(r, radius);
    subtract(cursor, middle, r);
    m_.add(limit, middle, r);
    subtract(width, b, a);
    m_.copy(period, width);
    if (boundary == "reflect")
      m_.add(period, width, width);
    m_.add(twice_b, b, b);
    while (m_.compare(cursor, limit) < 0) {
      m_.work(8192);
      std::uint64_t i = UINT64_MAX, j = UINT64_MAX;
      if ((boundary == "zero" || boundary == "replicate") &&
          m_.compare(cursor, a) < 0) {
        m_.copy(start, cursor);
        m_.copy(end, a);
        if (boundary == "replicate")
          i = j = 0;
      } else if ((boundary == "zero" || boundary == "replicate") &&
                 m_.compare(cursor, b) >= 0) {
        m_.copy(start, cursor);
        m_.copy(end, limit);
        if (boundary == "replicate")
          i = j = count - 1;
      } else {
        bool backward = false;
        m_.clear(shifted);
        m_.copy(folded, cursor);
        if (boundary == "reflect" || boundary == "wrap") {
          subtract(temporary, cursor, a);
          floor_ratio(quotient, temporary, period);
          integer_product(shifted, quotient, period);
          subtract(remainder, temporary, shifted);
          backward = boundary == "reflect" && m_.compare(remainder, width) >= 0;
          if (backward) {
            subtract(temporary, cursor, shifted);
            subtract(folded, twice_b, temporary);
          } else {
            m_.add(folded, a, remainder);
          }
        }
        const auto selected = segment(positions, count, folded, backward);
        raw(first, positions[selected]);
        raw(last, positions[selected + 1]);
        if (backward) {
          subtract(temporary, twice_b, last);
          m_.add(start, temporary, shifted);
          subtract(temporary, twice_b, first);
          m_.add(end, temporary, shifted);
          i = selected + 1;
          j = selected;
        } else {
          m_.add(start, first, shifted);
          m_.add(end, last, shifted);
          i = selected;
          j = selected + 1;
        }
      }
      if (m_.compare(end, cursor) <= 0)
        throw Status{ErrorCode::Internal,
                     "nonuniform boundary partition did not advance"};
      // No isolated contact adds a piece or a source endpoint dependency.
      pieces->emplace_back();
      auto& piece = pieces->back();
      m_.copy(piece.lower, cursor);
      m_.copy(piece.upper, m_.compare(end, limit) < 0 ? end : limit);
      m_.copy(piece.start, start);
      m_.copy(piece.end, end);
      piece.first = i;
      piece.last = j;
      m_.copy(cursor, piece.upper);
    }
  }
};
}  // namespace ps::plugin_internal::numeric_ops
