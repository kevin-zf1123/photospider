#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
namespace {
Parameters cast(const std::string& dtype,
                const std::string& overflow = "reject") {
  return {{"dtype", dtype},
          {"rounding", std::string("ties_even")},
          {"overflow", overflow}};
}
Parameters range(const std::string& dtype, double a, double b, double c,
                 double d) {
  auto p = cast(dtype);
  p.insert({{"src_min", a}, {"src_max", b}, {"dst_min", c}, {"dst_max", d}});
  return p;
}
}  // namespace
void numeric() {
  std::vector<std::uint8_t> samples(256);
  for (unsigned i = 0; i < 256; ++i)
    samples[i] = static_cast<std::uint8_t>(i);
  auto input = array(samples);
  auto result = evaluate({input}, {{1,
                                    "numeric.encode_range",
                                    {ps::WorkflowInputReference{1}},
                                    range("float32", 0, 255, 0, 1)},
                                   next(2, "numeric.encode_range", 1,
                                        range("uint8", 0, 1, 0, 255))});
  exact<std::uint8_t>(output(result), samples);
  for (const char* dtype : {"int64", "float32", "float64"})
    exact<std::uint8_t>(
        output(evaluate(
            {input},
            {{1, "numeric.cast", {ps::WorkflowInputReference{1}}, cast(dtype)},
             next(2, "numeric.cast", 1, cast("uint8"))})),
        samples);
  exact<std::int64_t>(
      output(operation("numeric.cast",
                       {array<double>({-2.5, -1.5, -.5, .5, 1.5, 2.5})},
                       cast("int64"))),
      {-2, -2, 0, 0, 2, 2});
  exact<std::int64_t>(
      output(operation("numeric.cast",
                       {array<std::int64_t>({INT64_MIN, INT64_MAX})},
                       cast("int64"))),
      {INT64_MIN, INT64_MAX});
  rejected(operation("numeric.cast", {array<double>({0x1p63})}, cast("int64")),
           ps::ErrorCode::OperationFailed);
  exact<std::int64_t>(
      output(operation("numeric.cast", {array<double>({0x1p63})},
                       cast("int64", "clip"))),
      {INT64_MAX});
  rejected(operation("numeric.cast", {array<double>({256})}, cast("uint8")),
           ps::ErrorCode::OperationFailed);
  for (double bad : {std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
    rejected(operation("numeric.cast", {array<double>({bad})}, cast("int64")),
             ps::ErrorCode::OperationFailed);
    rejected(operation("numeric.encode_range", {array<double>({bad})},
                       range("float32", 0, 1, 0, 255)),
             ps::ErrorCode::OperationFailed);
  }
  exact<float>(output(operation("numeric.subtract", {array<float>({3, 2, 1}),
                                                     array<float>({4, 4, 4})})),
               {-1, -2, -3});
  exact<double>(output(operation("numeric.mean", {array<float>({1, 2, 3})})),
                {2});
  auto variance =
      output(operation("numeric.variance", {array<float>({1, 2, 3})}));
  double v;
  std::memcpy(&v, variance.bytes().data(), 8);
  require(std::abs(v - 2. / 3) < 1e-15, "variance oracle");
  std::cout << "cast-range uint8_roundtrip=256 ties_even=passed "
               "int64_bounds=passed nonfinite=rejected "
               "negative_ramp=[-1,-2,-3] mean=2 variance=2/3 oracle=passed\n";
}
}  // namespace foundations
