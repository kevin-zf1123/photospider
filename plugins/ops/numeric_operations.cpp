#include "plugin/numeric_operations.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "numeric_common.hpp"  // NOLINT(build/include_subdir)

namespace ps::plugin_internal {
namespace {
using numeric_internal::numeric_failure;
using numeric_internal::read;
using numeric_internal::visit;
Status argument(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
ElementType target_type(const std::string& name) {
  return name == "uint8"     ? ElementType::UInt8
         : name == "int64"   ? ElementType::Int64
         : name == "float32" ? ElementType::Float32
                             : ElementType::Float64;
}
Result<Value> publish(MutableValue value, const CancellationToken& token) {
  if (token.cancelled()) {
    Status status;
    status.code = ErrorCode::Cancelled;
    return Result<Value>(status);
  }
  return std::move(value).publish();
}
/** @brief Checked conversion never routes integer sources through binary64. */
template <class Destination, class Source>
bool convert(Source input, Destination* output, bool clip) {
  if constexpr (std::is_integral_v<Destination>) {
    if constexpr (std::is_integral_v<Source>) {
      if constexpr (std::is_same_v<Destination, std::uint8_t> &&
                    std::is_same_v<Source, std::int64_t>) {
        if (input < 0 || input > 255) {
          if (!clip)
            return false;
          *output = input < 0 ? 0 : 255;
          return true;
        }
      }
      *output = static_cast<Destination>(input);
    } else {
      if (!std::isfinite(input))
        return false;
      const double rounded = std::nearbyint(static_cast<double>(input));
      const double lower =
          std::is_same_v<Destination, std::uint8_t> ? 0 : -0x1p63;
      // Int64's positive endpoint is exclusive, not double(INT64_MAX).
      const bool high = std::is_same_v<Destination, std::uint8_t>
                            ? rounded > 255
                            : rounded >= 0x1p63;
      if (rounded < lower || high) {
        if (!clip)
          return false;
        *output = rounded < lower ? std::numeric_limits<Destination>::lowest()
                                  : std::numeric_limits<Destination>::max();
      } else {
        *output = static_cast<Destination>(rounded);
      }
    }
  } else {
    if constexpr (std::is_floating_point_v<Source> &&
                  std::numeric_limits<Destination>::max_exponent <
                      std::numeric_limits<Source>::max_exponent) {
      if (std::isfinite(input) &&
          (input >
               static_cast<Source>(std::numeric_limits<Destination>::max()) ||
           input <
               -static_cast<Source>(std::numeric_limits<Destination>::max()))) {
        if (!clip)
          return false;
        *output = input < 0 ? -std::numeric_limits<Destination>::max()
                            : std::numeric_limits<Destination>::max();
        return true;
      }
    }
    *output = static_cast<Destination>(input);
  }
  return true;
}
template <class Source>
bool write_cast(Source input, ElementType target, std::uint8_t* output,
                bool clip) {
  switch (target) {
    case ElementType::UInt8: {
      std::uint8_t number;
      if (!convert(input, &number, clip))
        return false;
      std::memcpy(output, &number, 1);
      return true;
    }
    case ElementType::Int64: {
      std::int64_t number;
      if (!convert(input, &number, clip))
        return false;
      std::memcpy(output, &number, 8);
      return true;
    }
    case ElementType::Float32: {
      float number;
      if (!convert(input, &number, clip))
        return false;
      std::memcpy(output, &number, 4);
      return true;
    }
    case ElementType::Float64: {
      double number;
      if (!convert(input, &number, clip))
        return false;
      std::memcpy(output, &number, 8);
      return true;
    }
  }
  return false;
}
// Error-free addition for finite operands whose sum is representable.
struct Split {
  double high;
  double low;
};
Split two_sum(double a, double b) {
  const double high = a + b;
  if (!std::isfinite(high))
    return {high, 0};
  const double b_virtual = high - a;
  return {high, (a - (high - b_virtual)) + (b - b_virtual)};
}
// Accumulate a finite term into two components, returning only the magnitude
// of the third component that cannot be retained.
double add_split(Split* accumulator, double term) {
  const auto high = two_sum(accumulator->high, term);
  const auto low = two_sum(accumulator->low, high.low);
  const auto merged = two_sum(high.high, low.high);
  const auto tail = two_sum(merged.low, low.low);
  *accumulator = two_sum(merged.high, tail.high);
  return std::abs(tail.low);
}
struct Range {
  double src_min = 0, src_max = 1, dst_min = 0, dst_max = 1;
  bool identity() const { return src_min == dst_min && src_max == dst_max; }
  double map(double value) const {
    if (value == src_min)
      return dst_min;
    if (value == src_max)
      return dst_max;
    double a = src_min, b = src_max, c = dst_min, d = dst_max;
    if (!std::isfinite(b - a) || !std::isfinite(d - c)) {
      a *= .5;
      b *= .5;
      c *= .5;
      d *= .5;
      if (a * 2 != src_min || b * 2 != src_max || c * 2 != dst_min ||
          d * 2 != dst_max)
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto source = two_sum(b, -a);
    const auto target = two_sum(d, -c);
    const double slope = target.high / source.high;
    if (!std::isfinite(slope) || slope == 0)
      return std::numeric_limits<double>::quiet_NaN();
    // Retain the quotient remainder instead of folding it into a rounded
    // global intercept. A nearby anchor also reduces cancellation.
    const double remainder = std::fma(-slope, source.high, target.high) +
                             target.low - slope * source.low;
    const double slope_low = remainder / source.high;
    const bool first = std::abs(value - src_min) <= std::abs(value - src_max);
    const double anchor = first ? src_min : src_max;
    const double output_anchor = first ? dst_min : dst_max;
    double factor = 1;
    Split difference;
    if (!std::isfinite(value - anchor)) {
      if (value * .5 * 2 != value || anchor * .5 * 2 != anchor)
        return std::numeric_limits<double>::quiet_NaN();
      difference = two_sum(value * .5, -anchor * .5);
      factor = 2;
    } else {
      difference = two_sum(value, -anchor);
    }
    double product = difference.high * slope;
    if (!std::isfinite(product) && factor == 1) {
      if (difference.high * .5 * 2 != difference.high ||
          difference.low * .5 * 2 != difference.low)
        return std::numeric_limits<double>::quiet_NaN();
      difference.high *= .5;
      difference.low *= .5;
      factor = 2;
      product = difference.high * slope;
    }
    if (!std::isfinite(product))
      return product;
    const double quotient_correction = difference.high * slope_low;
    const double cross_correction = difference.low * slope_low;
    const double low_product = difference.low * slope;
    const double scaled_anchor = output_anchor / factor;
    if (scaled_anchor * factor != output_anchor)
      return std::numeric_limits<double>::quiet_NaN();
    auto sum = two_sum(product, scaled_anchor);
    if (!std::isfinite(sum.high))
      return sum.high;
    double discarded = 0;
    for (const double term :
         {std::fma(difference.high, slope, -product), quotient_correction,
          std::fma(difference.high, slope_low, -quotient_correction),
          low_product, std::fma(difference.low, slope, -low_product),
          cross_correction,
          std::fma(difference.low, slope_low, -cross_correction)}) {
      discarded += add_split(&sum, term);
    }
    const double result = sum.high + sum.low;
    // Only actual discarded terms and uncertainty in the quotient remainder
    // matter. Exact low components (such as the +2 beside DBL_MAX) are data.
    const double remainder_scale =
        (std::abs(std::fma(-slope, source.high, target.high)) +
         std::abs(target.low) + std::abs(slope * source.low)) /
        source.high;
    const double slope_uncertainty = std::numeric_limits<double>::epsilon() *
                                     (remainder_scale + std::abs(slope_low)) *
                                     8;
    const double uncertainty = discarded +
                               std::abs(difference.high) * slope_uncertainty +
                               std::abs(difference.low) * slope_uncertainty;
    const double ulp = std::nextafter(std::abs(result),
                                      std::numeric_limits<double>::infinity()) -
                       std::abs(result);
    if (std::isfinite(result) && uncertainty > ulp)
      return std::numeric_limits<double>::quiet_NaN();
    return result * factor;
  }
};
template <class Source>
Status cast_samples(const OperationInvocation& call, MutableValue* output,
                    ElementType target, bool clip, bool encode,
                    const Range& range) {
  const auto& input = call.inputs[0];
  const auto width = Value::element_size(target);
  return visit(
      input, call.cancellation, [&](auto index, const auto& coordinate) {
        auto* destination = output->data() + index * width;
        if (!encode && input.descriptor().element_type == target) {
          std::memcpy(
              destination,
              input.bytes().data() + input.byte_address(coordinate).value(),
              width);
          return Status::success();
        }
        const Source value = read<Source>(input, coordinate);
        if constexpr (std::is_floating_point_v<Source>)
          if (encode && !std::isfinite(value))
            return numeric_failure(index, "range input is nonfinite");
        if (!encode || range.identity())
          return write_cast(value, target, destination, clip)
                     ? Status::success()
                     : numeric_failure(
                           index, "cast overflow or nonfinite integer input");
        double mapped = range.map(static_cast<double>(value));
        if (clip && std::isinf(mapped))
          mapped = std::copysign(std::numeric_limits<double>::max(), mapped);
        if (!std::isfinite(mapped) ||
            !write_cast(mapped, target, destination, clip))
          return numeric_failure(index, "range overflow or nonfinite result");
        return Status::success();
      });
}
Result<Value> cast(const OperationInvocation& call, bool encode) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(argument("numeric environment unavailable"));
  if (std::get<std::string>(call.parameters.at("rounding")) != "ties_even")
    return Result<Value>(argument("rounding must be ties_even"));
  const auto overflow = std::get<std::string>(call.parameters.at("overflow"));
  if (overflow != "reject" && overflow != "clip")
    return Result<Value>(argument("overflow must be reject or clip"));
  Range range;
  if (encode) {
    range = {std::get<double>(call.parameters.at("src_min")),
             std::get<double>(call.parameters.at("src_max")),
             std::get<double>(call.parameters.at("dst_min")),
             std::get<double>(call.parameters.at("dst_max"))};
    if (!(range.src_min < range.src_max) || !(range.dst_min < range.dst_max))
      return Result<Value>(
          argument("range endpoints must be strictly increasing"));
  }
  const auto target =
      target_type(std::get<std::string>(call.parameters.at("dtype")));
  auto made =
      MutableValue::allocate({target, call.inputs[0].descriptor().shape},
                             call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  Status status;
  switch (call.inputs[0].descriptor().element_type) {
    case ElementType::UInt8:
      status = cast_samples<std::uint8_t>(call, &output, target,
                                          overflow == "clip", encode, range);
      break;
    case ElementType::Int64:
      status = cast_samples<std::int64_t>(call, &output, target,
                                          overflow == "clip", encode, range);
      break;
    case ElementType::Float32:
      status = cast_samples<float>(call, &output, target, overflow == "clip",
                                   encode, range);
      break;
    case ElementType::Float64:
      status = cast_samples<double>(call, &output, target, overflow == "clip",
                                    encode, range);
      break;
  }
  if (!status.ok())
    return Result<Value>(status);
  return publish(std::move(output), call.cancellation);
}
template <class Number>
Result<Value> arithmetic(const OperationInvocation& call, unsigned kind) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(argument("numeric environment unavailable"));
  double minimum = 0, maximum = 0;
  if (kind == 4) {
    minimum = std::get<double>(call.parameters.at("min"));
    maximum = std::get<double>(call.parameters.at("max"));
    if (minimum > maximum)
      return Result<Value>(argument("clamp min exceeds max"));
  }
  auto made = MutableValue::allocate(call.inputs[0].descriptor(),
                                     call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  auto status = visit(
      call.inputs[0], call.cancellation,
      [&](auto index, const auto& coordinate) {
        const Number a = read<Number>(call.inputs[0], coordinate);
        const Number b =
            kind == 4 ? Number{} : read<Number>(call.inputs[1], coordinate);
        if (!std::isfinite(a) || !std::isfinite(b))
          return numeric_failure(index, "arithmetic input is nonfinite");
        if (kind == 3 && b == 0)
          return numeric_failure(index, "division by zero");
        Number result;
        if (kind == 4) {
          const double bounded =
              std::clamp(static_cast<double>(a), minimum, maximum);
          if (!convert(bounded, &result, false))
            return numeric_failure(index, "clamp result outside dtype range");
        } else {
          result = kind == 0   ? a + b
                   : kind == 1 ? a - b
                   : kind == 2 ? a * b
                               : a / b;
        }
        if (!std::isfinite(result))
          return numeric_failure(index, "arithmetic result is nonfinite");
        std::memcpy(output.data() + index * sizeof(Number), &result,
                    sizeof(Number));
        return Status::success();
      });
  if (!status.ok())
    return Result<Value>(status);
  return publish(std::move(output), call.cancellation);
}
template <class Number>
Result<Value> reduction(const OperationInvocation& call, bool variance) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(argument("numeric environment unavailable"));
  const auto& input = call.inputs[0];
  auto count = input.region().element_count();
  if (!count.ok())
    return Result<Value>(count.status());
  double sum = 0;
  auto status =
      visit(input, call.cancellation, [&](auto index, const auto& coordinate) {
        const double value = read<Number>(input, coordinate);
        if (!std::isfinite(value))
          return numeric_failure(index, "reduction input is nonfinite");
        sum += value;
        return std::isfinite(sum)
                   ? Status::success()
                   : numeric_failure(index, "reduction sum overflow");
      });
  if (!status.ok())
    return Result<Value>(status);
  const double mean = sum / static_cast<double>(count.value());
  double result = mean;
  if (variance) {
    sum = 0;
    status = visit(
        input, call.cancellation, [&](auto index, const auto& coordinate) {
          const double difference =
              static_cast<double>(read<Number>(input, coordinate)) - mean;
          const double square = difference * difference;
          sum += square;
          return std::isfinite(sum)
                     ? Status::success()
                     : numeric_failure(index, "variance overflow");
        });
    if (!status.ok())
      return Result<Value>(status);
    result = sum / static_cast<double>(count.value());
  }
  auto made = MutableValue::allocate({ElementType::Float64, {1}},
                                     call.output_region, call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  std::memcpy(output.data(), &result, sizeof(result));
  return publish(std::move(output), call.cancellation);
}
}  // namespace
Status register_numeric_operations(OperationRegistry* registry) {
  const double maximum = std::numeric_limits<double>::max();
  for (bool encode : {false, true}) {
    OperationDefinition operation;
    operation.key = encode ? "numeric.encode_range" : "numeric.cast";
    auto& t = operation.traits;
    t.input_count = 1;
    t.input_schema.resize(1);
    t.shape_rule = OperationShapeRule::PreserveFirstInput;
    t.output_dtype_rule = OperationDtypeRule::Parameter;
    t.output_dtype_parameter = "dtype";
    t.requires_dense_output = true;
    t.parameter_schema = {{"dtype", OperationParameterType::String, true},
                          {"rounding", OperationParameterType::String, true},
                          {"overflow", OperationParameterType::String, true}};
    if (encode)
      for (const char* key : {"src_min", "src_max", "dst_min", "dst_max"})
        t.parameter_schema.push_back({key, OperationParameterType::Float64,
                                      true, true, -maximum, maximum});
    operation.callback = [encode](const OperationInvocation& call) {
      return cast(call, encode);
    };
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  const char* names[] = {
      "numeric.add",   "numeric.subtract", "numeric.multiply", "numeric.divide",
      "numeric.clamp", "numeric.mean",     "numeric.variance"};
  for (unsigned kind = 0; kind < 7; ++kind) {
    OperationDefinition operation;
    operation.key = names[kind];
    auto& t = operation.traits;
    t.input_schema.resize(1);
    t.input_schema[0].element_type_mask = 12;
    t.requires_dense_output = true;
    if (kind < 4) {
      t.repeated_minimum = t.repeated_maximum = 2;
      t.repeated_match = 1;
    } else {
      t.input_count = 1;
    }
    if (kind < 5) {
      t.shape_rule = OperationShapeRule::PreserveFirstInput;
      t.output_dtype_rule = OperationDtypeRule::Input;
    }
    if (kind == 4)
      t.parameter_schema = {{"min", OperationParameterType::Float64, true, true,
                             -maximum, maximum},
                            {"max", OperationParameterType::Float64, true, true,
                             -maximum, maximum}};
    operation.callback = [kind](const OperationInvocation& call) {
      const bool fp32 =
          call.inputs[0].descriptor().element_type == ElementType::Float32;
      if (kind < 5)
        return fp32 ? arithmetic<float>(call, kind)
                    : arithmetic<double>(call, kind);
      return fp32 ? reduction<float>(call, kind == 6)
                  : reduction<double>(call, kind == 6);
    };
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
