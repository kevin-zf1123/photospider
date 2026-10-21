#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "common/numeric_common.hpp"  // NOLINT(build/include_subdir)
#include "photospider/plugin/fixed_invocation.hpp"

namespace ps::basic_internal {
struct Failure {
  Status status;
};
inline void require(bool condition, ErrorCode code, const char* message) {
  if (!condition)
    throw Failure{Status::failure(code, message)};
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw Failure{result.status()};
  return result.take_value();
}
inline void choice(const std::string& value, const char* a, const char* b) {
  require(value == a || value == b, ErrorCode::InvalidArgument,
          "unknown basic operation parameter choice");
}
// Invocation-local bounded failure state. Numerical guards never allocate or
// throw; static metadata validation uses the separate require() helper above.
struct Arithmetic final {
  ErrorCode code = ErrorCode::Ok;
  const char* message = nullptr;
  bool check(bool condition, ErrorCode failure, const char* detail) {
    if (!condition && code == ErrorCode::Ok) {
      code = failure;
      message = detail;
    }
    return condition;
  }
  double finite(double number) {
    return check(std::isfinite(number), ErrorCode::OperationFailed,
                 "nonfinite basic operation intermediate")
               ? number
               : 0;
  }
  // Stable convex interpolation uses both distances independently. Scaling only
  // when the interval difference overflows retains subnormal ordinary
  // intervals.
  double fraction(double x, double a, double b) {
    if (!check(a < b, ErrorCode::InvalidArgument, "interval must increase"))
      return 0;
    double width = b - a;
    double distance = x - a;
    if (!std::isfinite(width) || !std::isfinite(distance)) {
      width = b * .5 - a * .5;
      distance = x * .5 - a * .5;
    }
    const double result = finite(distance / width);
    if (!check(x <= a || x >= b || (result > 0 && result < 1),
               ErrorCode::OperationFailed,
               "interval coordinate loses precision"))
      return 0;
    return result;
  }
  // Sum unnormalized weighted products before division. Independently rounded
  // normalized weights can turn exact cancellation into an enormous residual.
  double interpolate(double x, double x0, double x1, double y0, double y1) {
    if (x == x0)
      return y0;
    if (x == x1)
      return y1;
    if (y0 == y1)
      return y0;
    using numeric_internal::add_split;
    using numeric_internal::two_sum;
    auto width = two_sum(x1, -x0);
    auto left = two_sum(x, -x0);
    auto right = two_sum(x1, -x);
    if (!std::isfinite(width.high)) {
      // Halving is exact except at the subnormal boundary; reject a lost query.
      if (!check(x == 0 || (x * .5) * 2 == x, ErrorCode::OperationFailed,
                 "interpolation scaling loses query"))
        return 0;
      width = two_sum(x1 * .5, -x0 * .5);
      left = two_sum(x * .5, -x0 * .5);
      right = two_sum(x1 * .5, -x * .5);
    }
    int exponent = 0;
    std::frexp(width.high, &exponent);
    auto scale = [&](numeric_internal::Split v) {
      const double high = std::scalbn(v.high, -exponent);
      const double low = std::scalbn(v.low, -exponent);
      if (!check(std::scalbn(high, exponent) == v.high &&
                     std::scalbn(low, exponent) == v.low,
                 ErrorCode::OperationFailed,
                 "interpolation scaling loses distance"))
        return numeric_internal::Split{0, 0};
      return numeric_internal::Split{high, low};
    };
    width = scale(width);
    left = scale(left);
    right = scale(right);
    if (!check(width.high > 0 && left.high >= 0 && right.high >= 0,
               ErrorCode::OperationFailed, "invalid interpolation distances"))
      return 0;
    // Normalize products separately, then align them to a common exponent.
    // Normalizing only distances can underflow even an exactly representable
    // subnormal output before the final division restores its magnitude.
    struct Product {
      numeric_internal::Split mantissa;
      int exponent = 0;
    };
    auto product = [](double a, double b) {
      if (a == 0 || b == 0)
        return Product{{0, 0}, 0};
      int ae = 0, be = 0;
      const double am = std::frexp(a, &ae), bm = std::frexp(b, &be);
      const double high = am * bm;
      return Product{{high, std::fma(am, bm, -high)}, ae + be};
    };
    const std::array<Product, 4> products = {
        product(y0, right.high), product(y1, left.high), product(y0, right.low),
        product(y1, left.low)};
    int product_exponent = std::numeric_limits<int>::min();
    for (const auto& p : products)
      if (p.mantissa.high != 0)
        product_exponent = std::max(product_exponent, p.exponent);
    if (product_exponent == std::numeric_limits<int>::min())
      return 0;
    numeric_internal::Split numerator{0, 0};
    double error = 0;
    for (const auto& p : products) {
      if (p.mantissa.high == 0)
        continue;
      const int shift = p.exponent - product_exponent;
      const double high = std::scalbn(p.mantissa.high, shift);
      const double low = std::scalbn(p.mantissa.low, shift);
      if (!check(std::scalbn(high, -shift) == p.mantissa.high &&
                     std::scalbn(low, -shift) == p.mantissa.low,
                 ErrorCode::OperationFailed,
                 "interpolation product alignment loses precision"))
        return 0;
      error += add_split(&numerator, high);
      error += add_split(&numerator, low);
    }
    const double high = finite(numerator.high / width.high);
    auto remainder =
        two_sum(std::fma(-high, width.high, numerator.high), numerator.low);
    error += add_split(&remainder, -high * width.low);
    error +=
        add_split(&remainder, std::fma(-high, width.low, high * width.low));
    const double result =
        finite(high + (remainder.high + remainder.low) / width.high);
    const double ulp = std::max(std::numeric_limits<double>::denorm_min(),
                                std::abs(result - std::nextafter(result, 0.)));
    if (!check(error / width.high <= ulp / 8, ErrorCode::OperationFailed,
               "interpolation precision cannot be represented"))
      return 0;
    return finite(std::scalbn(result, product_exponent));
  }
};
}  // namespace ps::basic_internal
