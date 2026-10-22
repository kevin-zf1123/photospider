#pragma once

#include "03-generation/math.hpp"
#include "04-mask-morphology/common.hpp"

namespace ps::plugin_internal::mask {
using Math = generation::Math;
using Directed = generation::Directed;
using Interval = generation::Interval;

struct Response final {
  double inner, outer;
  bool smooth;
  explicit Response(const Parameters& p)
      : inner(real(p, "inner")),
        outer(real(p, "outer")),
        smooth(text(p, "curve") == "smoothstep") {
    parameter(text(p, "curve") == "linear" || smooth, "unknown mask curve");
  }
  std::uint64_t rational(Math& math, const Rational& distance,
                         bool narrow) const;
  std::uint64_t squared(Math& math, const Rational& distance_squared,
                        bool narrow) const;
  std::uint64_t quadratic(Math& math, const Rational& a, const Rational& b,
                          const Rational& v, bool narrow) const;
  void interval(Math& math, Interval output, Interval distance) const;
};

// Exact rational comparisons resolve ties of A+B*sqrt(v), including an exact
// IEEE midpoint; interval refinement alone cannot resolve such a midpoint.
std::uint64_t coverage_quadratic(Math& math, const Rational& a,
                                 const Rational& b, const Rational& v,
                                 bool narrow);
std::uint64_t ciede2000(Math& math, const std::array<Rational, 3>& first,
                        const std::array<Rational, 3>& second,
                        const std::array<double, 3>& factors,
                        const Response& response, bool narrow);
}  // namespace ps::plugin_internal::mask
