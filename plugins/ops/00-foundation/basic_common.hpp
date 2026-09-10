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

#include "00-foundation/numeric_common.hpp"  // NOLINT(build/include_subdir)

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
inline void poll(const OperationInvocation& call) {
  require(!call.cancellation.cancelled(), ErrorCode::Cancelled,
          "basic operation cancelled");
}
inline double finite(double number) {
  require(std::isfinite(number), ErrorCode::OperationFailed,
          "nonfinite basic operation input or intermediate");
  return number;
}
inline double parameter(const OperationInvocation& call, const char* key) {
  return std::get<double>(call.parameters.at(key));
}
inline std::uint64_t integer(const OperationInvocation& call, const char* key) {
  return static_cast<std::uint64_t>(
      std::get<std::int64_t>(call.parameters.at(key)));
}
inline const std::string& text(const OperationInvocation& call,
                               const char* key) {
  return std::get<std::string>(call.parameters.at(key));
}
inline void choice(const std::string& value, const char* a, const char* b) {
  require(value == a || value == b, ErrorCode::InvalidArgument,
          "unknown basic operation parameter choice");
}
template <class T>
T read(const Value& value, const std::vector<std::uint64_t>& coordinate) {
  const T result = numeric_internal::read<T>(value, coordinate);
  finite(result);
  return result;
}
template <class T>
void store(std::uint8_t* bytes, std::uint64_t index, double number) {
  finite(number);
  require(std::abs(number) <= std::numeric_limits<T>::max(),
          ErrorCode::OperationFailed, "basic output outside dtype range");
  const T result = static_cast<T>(number);
  std::memcpy(bytes + index * sizeof(T), &result, sizeof(T));
}
inline void store_count(std::uint8_t* bytes, std::uint64_t index,
                        std::int64_t count) {
  std::memcpy(bytes + index * 8, &count, 8);
}
inline std::int64_t load_count(const std::uint8_t* bytes, std::uint64_t index) {
  std::int64_t count;
  std::memcpy(&count, bytes + index * 8, 8);
  return count;
}
inline void same_type(const Value& a, const Value& b) {
  require(a.descriptor().element_type == b.descriptor().element_type,
          ErrorCode::TypeMismatch, "basic inputs must share dtype");
}
inline void generic(const Value& value) {
  require(value.facets().empty(), ErrorCode::TypeMismatch,
          "table or controls require generic array");
}
inline void field(const Value& value) {
  require(value.descriptor().shape.size() == 2, ErrorCode::TypeMismatch,
          "field requires rank two");
  if (value.facets().empty())
    return;
  require(value.facets().size() == 1, ErrorCode::TypeMismatch,
          "field requires scalar or coverage semantics");
  auto s = take(decode_semantic(value.facets()[0]));
  require(s.kind == SemanticKind::ScalarField ||
              value.facets()[0].payload ==
                  take(encode_semantic(coverage_semantics())).payload,
          ErrorCode::TypeMismatch,
          "field requires scalar or coverage semantics");
}
// Stable convex interpolation uses both distances independently. Scaling only
// when the interval difference overflows retains subnormal ordinary intervals.
inline double fraction(double x, double a, double b) {
  require(a < b, ErrorCode::InvalidArgument, "interval must increase");
  double width = b - a;
  double distance = x - a;
  if (!std::isfinite(width) || !std::isfinite(distance)) {
    width = b * .5 - a * .5;
    distance = x * .5 - a * .5;
  }
  const double result = finite(distance / width);
  require(x <= a || x >= b || (result > 0 && result < 1),
          ErrorCode::OperationFailed, "interval coordinate loses precision");
  return result;
}
// Sum unnormalized weighted products before division. Independently rounded
// normalized weights can turn exact cancellation into an enormous residual.
inline double interpolate(double x, double x0, double x1, double y0,
                          double y1) {
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
    require(x == 0 || (x * .5) * 2 == x, ErrorCode::OperationFailed,
            "interpolation scaling loses query");
    width = two_sum(x1 * .5, -x0 * .5);
    left = two_sum(x * .5, -x0 * .5);
    right = two_sum(x1 * .5, -x * .5);
  }
  int exponent;
  std::frexp(width.high, &exponent);
  auto scale = [&](numeric_internal::Split v) {
    const double high = std::scalbn(v.high, -exponent);
    const double low = std::scalbn(v.low, -exponent);
    require(std::scalbn(high, exponent) == v.high &&
                std::scalbn(low, exponent) == v.low,
            ErrorCode::OperationFailed, "interpolation scaling loses distance");
    return numeric_internal::Split{high, low};
  };
  width = scale(width);
  left = scale(left);
  right = scale(right);
  require(width.high > 0 && left.high >= 0 && right.high >= 0,
          ErrorCode::OperationFailed, "invalid interpolation distances");
  // Normalize products separately, then align them to a common exponent.
  // Normalizing only distances can underflow even an exactly representable
  // subnormal output before the final division restores its magnitude.
  struct Product {
    numeric_internal::Split mantissa;
    int exponent;
  };
  auto product = [](double a, double b) {
    if (a == 0 || b == 0)
      return Product{{0, 0}, 0};
    int ae, be;
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
    require(std::scalbn(high, -shift) == p.mantissa.high &&
                std::scalbn(low, -shift) == p.mantissa.low,
            ErrorCode::OperationFailed,
            "interpolation product alignment loses precision");
    error += add_split(&numerator, high);
    error += add_split(&numerator, low);
  }
  const double high = finite(numerator.high / width.high);
  auto remainder =
      two_sum(std::fma(-high, width.high, numerator.high), numerator.low);
  error += add_split(&remainder, -high * width.low);
  error += add_split(&remainder, std::fma(-high, width.low, high * width.low));
  const double result =
      finite(high + (remainder.high + remainder.low) / width.high);
  const double ulp = std::max(std::numeric_limits<double>::denorm_min(),
                              std::abs(result - std::nextafter(result, 0.)));
  require(error / width.high <= ulp / 8, ErrorCode::OperationFailed,
          "interpolation precision cannot be represented");
  return finite(std::scalbn(result, product_exponent));
}
inline double blend(double a, double b, double t) {
  return interpolate(t, 0, 1, a, b);
}
template <class Function>
void each(const Region& region, const OperationInvocation& call,
          Function function) {
  const auto count = take(region.element_count());
  const auto& dims = region.dimensions();
  std::vector<std::uint64_t> coordinate;
  for (const auto& d : dims)
    coordinate.push_back(d.offset);
  for (std::uint64_t i = 0; i < count; ++i) {
    if ((i & 255U) == 0)
      poll(call);
    function(i, coordinate);
    for (std::size_t axis = dims.size(); axis; --axis) {
      if (++coordinate[axis - 1] <
          dims[axis - 1].offset + dims[axis - 1].extent)
        break;
      coordinate[axis - 1] = dims[axis - 1].offset;
    }
  }
}
// Offset without converting the complete logical coordinate to signed integer.
inline bool shifted(std::uint64_t position, std::int64_t offset,
                    std::uint64_t length, bool clamp, std::uint64_t* result) {
  if (offset < 0) {
    const auto amount = static_cast<std::uint64_t>(-(offset + 1)) + 1;
    if (amount > position) {
      *result = 0;
      return clamp;
    }
    *result = position - amount;
  } else {
    const auto amount = static_cast<std::uint64_t>(offset);
    if (amount >= length - position) {
      *result = length - 1;
      return clamp;
    }
    *result = position + amount;
  }
  return true;
}
}  // namespace ps::basic_internal
