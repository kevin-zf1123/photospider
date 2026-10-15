#include "photospider/ops/composite/layer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "common/floating_environment.hpp"
#include "photospider/ops/errors.hpp"

namespace ps {
namespace {
Status bad(const char* detail) {
  return {ErrorCode::TypeMismatch, detail, ops_error::InvalidAssociation};
}
Status computed(Status status) {
  if (!status.ok()) {
    status.code = ErrorCode::OperationFailed;
    if (status.detail.domain_code == ops_error::InvalidAssociation.value)
      status.detail.domain_code = ops_error::AssociationUnderflow.value;
  }
  return status;
}
Status overflow() {
  return {ErrorCode::OperationFailed, "nonfinite layer arithmetic",
          FailureReason::ArithmeticOverflow};
}
Status environment_error() {
  return {ErrorCode::OperationFailed,
          "strict layer floating environment unavailable"};
}
template <class T>
bool finite3(const std::array<T, 3>& values) {
  return std::all_of(values.begin(), values.end(),
                     [](T x) { return std::isfinite(x); });
}
template <class T>
bool zero3(const std::array<T, 3>& values) {
  return std::all_of(values.begin(), values.end(), [](T x) { return x == 0; });
}
template <class T>
T add(T a, T b) {
  volatile T result = a + b;
  return result;
}
template <class T>
T mul(T a, T b) {
  volatile T result = a * b;
  return result;
}
float transmittance(float a) {
  volatile float t = 1.0F - a;
  return t;
}
Status response_valid(const LayerResponsePixel& value) {
  return finite3(value.q) && std::isfinite(value.t) && value.t >= 0 &&
                 value.t <= 1
             ? Status::success()
             : bad("invalid layer response");
}
}  // namespace
Status validate_coverage(const CoveragePixel& value) {
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active())
    return environment_error();
  if (!finite3(value.p) || !std::isfinite(value.a))
    return overflow();
  if (value.a < 0 || value.a > 1 || (value.a == 0 && !zero3(value.p)))
    return bad("coverage requires A in [0,1] and A=0 implies P=0");
  return Status::success();
}
Status validate_layer(const LayerPixel& value) {
  auto status = validate_coverage(value.coverage);
  if (!status.ok())
    return status;
  return finite3(value.emission) ? Status::success() : overflow();
}
Status validate_raw_rgba_sum(const RawRgbaSumPixel& value) {
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active())
    return environment_error();
  if (!finite3(value.p) || !std::isfinite(value.mass))
    return overflow();
  return value.mass >= 0 && (value.mass != 0 || zero3(value.p))
             ? Status::success()
             : bad("raw mass must be nonnegative and zero mass requires zero "
                   "P");
}
Status validate_weighted_layer_sum(const WeightedLayerSum& value) {
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active())
    return environment_error();
  const auto& n = value.components;
  for (double x : n)
    if (!std::isfinite(x))
      return overflow();
  if (n[7] < 0 || n[3] < 0 || n[3] > n[7])
    return bad("invalid weighted alpha or weight");
  if (n[3] == 0 && (n[0] != 0 || n[1] != 0 || n[2] != 0))
    return bad("zero alpha numerator requires zero P numerator");
  if (n[7] == 0) {
    for (unsigned i = 0; i < 7; ++i)
      if (n[i] != 0)
        return bad("zero weight requires all numerators zero");
  }
  return Status::success();
}
Status validate_layer_contribution(const LayerContribution& value) {
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active())
    return environment_error();
  const auto& n = value.components;
  if (!std::isfinite(n[7]) || n[7] < 0)
    return bad("invalid contribution weight");
  std::array<float, 7> v{};
  for (unsigned i = 0; i < 7; ++i) {
    if (!std::isfinite(n[i]) ||
        std::abs(n[i]) > std::numeric_limits<float>::max())
      return bad("contribution must carry finite binary32 samples");
    v[i] = static_cast<float>(n[i]);
    if (static_cast<double>(v[i]) != n[i])
      return bad("contribution sample is not binary32");
  }
  return validate_layer({{{v[0], v[1], v[2]}, v[3]}, {v[4], v[5], v[6]}});
}
Result<LayerPixel> layer_over(const LayerPixel& front, const LayerPixel& back) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<LayerPixel>(environment_error());
  for (const auto* p : {&front, &back}) {
    auto s = validate_layer(*p);
    if (!s.ok())
      return Result<LayerPixel>(s);
  }
  const auto t = transmittance(front.coverage.a);
  LayerPixel result;
  result.coverage.a = add(front.coverage.a, mul(t, back.coverage.a));
  for (unsigned i = 0; i < 3; ++i) {
    result.coverage.p[i] = add(front.coverage.p[i], mul(t, back.coverage.p[i]));
    result.emission[i] = add(front.emission[i], mul(t, back.emission[i]));
  }
  auto s = computed(validate_layer(result));
  return s.ok() ? Result<LayerPixel>(result) : Result<LayerPixel>(s);
}
Result<LayerPixel> layer_opacity(const LayerPixel& input, float opacity) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<LayerPixel>(environment_error());
  auto s = validate_layer(input);
  if (!s.ok())
    return Result<LayerPixel>(s);
  if (!std::isfinite(opacity) || opacity < 0 || opacity > 1)
    return Result<LayerPixel>(
        Status{ErrorCode::InvalidArgument, "opacity outside [0,1]"});
  LayerPixel result = input;
  result.coverage.a = mul(input.coverage.a, opacity);
  for (unsigned i = 0; i < 3; ++i)
    result.coverage.p[i] = mul(input.coverage.p[i], opacity);
  s = computed(validate_layer(result));
  return s.ok() ? Result<LayerPixel>(result) : Result<LayerPixel>(s);
}
Result<LayerPixel> layer_emit(const LayerPixel& input,
                              const std::array<float, 3>& emission, float gain,
                              bool behind) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<LayerPixel>(environment_error());
  auto s = validate_layer(input);
  if (!s.ok())
    return Result<LayerPixel>(s);
  if (!finite3(emission) || !std::isfinite(gain))
    return Result<LayerPixel>(bad("nonfinite emission input"));
  LayerPixel result = input;
  for (unsigned i = 0; i < 3; ++i) {
    auto term = mul(gain, emission[i]);
    if (!std::isfinite(term))
      return Result<LayerPixel>(overflow());
    if (behind)
      term = mul(transmittance(input.coverage.a), term);
    result.emission[i] = add(input.emission[i], term);
  }
  s = computed(validate_layer(result));
  return s.ok() ? Result<LayerPixel>(result) : Result<LayerPixel>(s);
}
Result<CoveragePixel> layer_flatten(const LayerPixel& input,
                                    const std::array<float, 3>& background) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<CoveragePixel>(environment_error());
  auto s = validate_layer(input);
  if (!s.ok())
    return Result<CoveragePixel>(s);
  if (!finite3(background))
    return Result<CoveragePixel>(bad("nonfinite opaque background"));
  CoveragePixel result;
  result.a = 1;
  for (unsigned i = 0; i < 3; ++i) {
    const float q = add(input.coverage.p[i], input.emission[i]);
    const float b = mul(transmittance(input.coverage.a), background[i]);
    if (!std::isfinite(q) || !std::isfinite(b))
      return Result<CoveragePixel>(overflow());
    result.p[i] = add(q, b);
  }
  s = computed(validate_coverage(result));
  return s.ok() ? Result<CoveragePixel>(result) : Result<CoveragePixel>(s);
}
Result<LayerResponsePixel> layer_response(const LayerPixel& input) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<LayerResponsePixel>(environment_error());
  auto s = validate_layer(input);
  if (!s.ok())
    return Result<LayerResponsePixel>(s);
  LayerResponsePixel result;
  result.t = transmittance(input.coverage.a);
  for (unsigned i = 0; i < 3; ++i)
    result.q[i] = add(input.coverage.p[i], input.emission[i]);
  if (!finite3(result.q))
    return Result<LayerResponsePixel>(overflow());
  return Result<LayerResponsePixel>(result);
}
Result<LayerResponsePixel> response_over(const LayerResponsePixel& front,
                                         const LayerResponsePixel& back) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<LayerResponsePixel>(environment_error());
  for (const auto* p : {&front, &back}) {
    auto s = response_valid(*p);
    if (!s.ok())
      return Result<LayerResponsePixel>(s);
  }
  LayerResponsePixel result;
  result.t = mul(front.t, back.t);
  for (unsigned i = 0; i < 3; ++i)
    result.q[i] = add(front.q[i], mul(front.t, back.q[i]));
  if (!finite3(result.q))
    return Result<LayerResponsePixel>(overflow());
  return Result<LayerResponsePixel>(result);
}
Result<RawRgbaSumPixel> raw_rgba_plus(const CoveragePixel& first,
                                      const CoveragePixel& second) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<RawRgbaSumPixel>(environment_error());
  for (const auto* p : {&first, &second}) {
    auto s = validate_coverage(*p);
    if (!s.ok())
      return Result<RawRgbaSumPixel>(s);
  }
  RawRgbaSumPixel result;
  result.mass = add(first.a, second.a);
  for (unsigned i = 0; i < 3; ++i)
    result.p[i] = add(first.p[i], second.p[i]);
  auto s = computed(validate_raw_rgba_sum(result));
  return s.ok() ? Result<RawRgbaSumPixel>(result) : Result<RawRgbaSumPixel>(s);
}
Result<CoveragePixel> raw_rgba_coverage(const RawRgbaSumPixel& input,
                                        bool cap_alpha_keep_color) {
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active())
    return Result<CoveragePixel>(environment_error());
  auto s = validate_raw_rgba_sum(input);
  if (!s.ok())
    return Result<CoveragePixel>(s);
  if (!cap_alpha_keep_color && input.mass > 1)
    return Result<CoveragePixel>(bad("raw mass exceeds coverage range"));
  return Result<CoveragePixel>(CoveragePixel{
      input.p, cap_alpha_keep_color ? std::min(input.mass, 1.0F) : input.mass});
}
Result<WeightedLayerSum> weighted_layer_leaf(const LayerPixel& input,
                                             double weight) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<WeightedLayerSum>(environment_error());
  auto s = validate_layer(input);
  if (!s.ok())
    return Result<WeightedLayerSum>(s);
  if (!std::isfinite(weight) || weight < 0)
    return Result<WeightedLayerSum>(Status{
        ErrorCode::InvalidArgument, "weight must be finite and nonnegative"});
  WeightedLayerSum result;
  auto& n = result.components;
  for (unsigned i = 0; i < 3; ++i) {
    n[i] = mul(static_cast<double>(input.coverage.p[i]), weight);
    n[i + 4] = mul(static_cast<double>(input.emission[i]), weight);
  }
  n[3] = mul(static_cast<double>(input.coverage.a), weight);
  n[7] = weight;
  s = computed(validate_weighted_layer_sum(result));
  return s.ok() ? Result<WeightedLayerSum>(result)
                : Result<WeightedLayerSum>(s);
}
Result<WeightedLayerSum> weighted_layer_add(const WeightedLayerSum& left,
                                            const WeightedLayerSum& right) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<WeightedLayerSum>(environment_error());
  for (const auto* p : {&left, &right}) {
    auto s = validate_weighted_layer_sum(*p);
    if (!s.ok())
      return Result<WeightedLayerSum>(s);
  }
  WeightedLayerSum result;
  for (unsigned i = 0; i < 8; ++i)
    result.components[i] = add(left.components[i], right.components[i]);
  auto s = computed(validate_weighted_layer_sum(result));
  return s.ok() ? Result<WeightedLayerSum>(result)
                : Result<WeightedLayerSum>(s);
}
Result<OptionalLayerPixel> weighted_layer_finalize(
    const WeightedLayerSum& input) {
  plugin_internal::FloatingEnvironment env;
  if (!env.active())
    return Result<OptionalLayerPixel>(environment_error());
  auto s = validate_weighted_layer_sum(input);
  if (!s.ok())
    return Result<OptionalLayerPixel>(s);
  OptionalLayerPixel result;
  const auto& n = input.components;
  if (n[7] == 0)
    return Result<OptionalLayerPixel>(result);
  std::array<float, 7> values{};
  for (unsigned i = 0; i < 7; ++i) {
    volatile double ratio = n[i] / n[7];
    volatile float rounded = static_cast<float>(ratio);
    values[i] = rounded;
  }
  for (unsigned i = 0; i < 3; ++i) {
    result.value.coverage.p[i] = values[i];
    result.value.emission[i] = values[i + 4];
  }
  result.value.coverage.a = values[3];
  result.valid = true;
  s = computed(validate_layer(result.value));
  return s.ok() ? Result<OptionalLayerPixel>(result)
                : Result<OptionalLayerPixel>(s);
}
}  // namespace ps
