#include "photospider/data/layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "data/input_validation.hpp"

namespace ps {
namespace {
Status bad(const char* detail) {
  return {ErrorCode::TypeMismatch, detail, FailureReason::InvalidAssociation};
}
Status computed(Status status) {
  if (!status.ok()) {
    status.code = ErrorCode::OperationFailed;
    if (status.reason == FailureReason::InvalidAssociation)
      status.reason = FailureReason::AssociationUnderflow;
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
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr const char* kIds[] = {"",
                                "photospider.layer",
                                "photospider.layer_response",
                                "photospider.raw_rgba_sum",
                                "photospider.layer_contributions",
                                "photospider.weighted_layer_sum",
                                "photospider.optional_layer"};
// NOLINTEND
unsigned kind(std::string_view id) {
  for (unsigned i = 1; i <= 6; ++i)
    if (id == kIds[i])
      return i;
  return 0;
}
bool dimensions(const LayerSpec& spec) {
  return spec.working_space == 1 && spec.height && spec.width &&
         spec.height <= static_cast<std::uint64_t>(INT64_MAX) / 64 / spec.width;
}
void word(ResourceVector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint64_t word(const ResourceVector<std::uint8_t>& bytes, unsigned offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
  return value;
}
Result<LayerSpec> parse(const SchemaTemplate& schema) {
  const auto k = kind(schema.id);
  if (!k || schema.version != 1 ||
      schema.publication != PublishPolicy::CompleteBundle ||
      schema.metadata.size() != 1 ||
      schema.metadata[0].key != "photospider.layer" ||
      schema.metadata[0].version != 1 ||
      schema.metadata[0].payload.size() != 24)
    return Result<LayerSpec>(bad("invalid layer schema identity"));
  const auto& bytes = schema.metadata[0].payload;
  const auto space = word(bytes, 16);
  LayerSpec spec{word(bytes, 0), word(bytes, 8), 1};
  if (space != 1 || !dimensions(spec) ||
      (k >= 4 && (spec.height != 1 || spec.width != 1)))
    return Result<LayerSpec>(bad("invalid layer dimensions or working space"));
  return Result<LayerSpec>(spec);
}
ResultExtent extent(ResultExtentKind k, std::uint64_t value = 1,
                    unsigned field = 0) {
  ResultExtent e;
  e.kind = k;
  e.value = value;
  e.field = field;
  return e;
}
bool same_extent(const ResultExtent& a, const ResultExtent& b) {
  return a.kind == b.kind && a.value == b.value && a.field == b.field &&
         a.input == 0 && a.axis == 0 && a.divisor == 1 && a.offset == 0;
}
}  // namespace
Status validate_coverage(const CoveragePixel& value) {
  input_internal::Float32Environment environment;
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
  input_internal::Float32Environment environment;
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
  input_internal::Float32Environment environment;
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
  input_internal::Float32Environment environment;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment environment;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
  input_internal::Float32Environment env;
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
bool has_layer_schema(std::string_view id) noexcept {
  return kind(id) != 0;
}
Result<SchemaTemplate> layer_schema(LayerRepresentation representation,
                                    const LayerSpec& spec) {
  const auto k = static_cast<unsigned>(representation);
  if (k < 1 || k > 6 || !dimensions(spec) ||
      (k >= 4 && (spec.height != 1 || spec.width != 1)))
    return Result<SchemaTemplate>(bad("invalid layer schema specification"));
  SchemaTemplate s;
  s.id = kIds[k];
  ResultFacet facet{"photospider.layer", 1, {}};
  word(&facet.payload, spec.height);
  word(&facet.payload, spec.width);
  word(&facet.payload, spec.working_space);
  s.metadata.push_back(std::move(facet));
  if (k <= 3) {
    s.domain = {extent(ResultExtentKind::Fixed, spec.height),
                extent(ResultExtentKind::Fixed, spec.width)};
  }
  const auto fixed = extent(ResultExtentKind::Fixed, spec.height * spec.width);
  auto field = [&](const char* key, ElementType type, ResultExtent rows,
                   std::uint64_t width) {
    s.fields.push_back({key, type, rows, {width}});
  };
  if (k == 1) {
    field("coverage", ElementType::Float32, fixed, 4);
    field("emission", ElementType::Float32, fixed, 3);
  }
  if (k == 2)
    field("response", ElementType::Float32, fixed, 4);
  if (k == 3)
    field("raw_sum", ElementType::Float32, fixed, 4);
  if (k == 4 || k == 5)
    field(k == 4 ? "contributions" : "weighted", ElementType::Float64,
          k == 4 ? extent(ResultExtentKind::RuntimeCount)
                 : extent(ResultExtentKind::Fixed),
          8);
  if (k == 6) {
    field("valid", ElementType::UInt8, extent(ResultExtentKind::Fixed), 1);
    field("coverage", ElementType::Float32,
          extent(ResultExtentKind::RuntimeCount), 4);
    field("emission", ElementType::Float32,
          extent(ResultExtentKind::FieldRows, 1, 1), 3);
  }
  return Result<SchemaTemplate>(std::move(s));
}
Status validate_layer_schema(const SchemaTemplate& schema) {
  const auto k = kind(schema.id);
  if (!k)
    return Status::success();
  auto decoded = parse(schema);
  if (!decoded.ok())
    return decoded.status();
  const auto spec = decoded.value();
  if (schema.domain.size() != (k <= 3 ? 2U : 0U))
    return bad("layer domain mismatch");
  if (k <= 3 && (!same_extent(schema.domain[0],
                              extent(ResultExtentKind::Fixed, spec.height)) ||
                 !same_extent(schema.domain[1],
                              extent(ResultExtentKind::Fixed, spec.width))))
    return bad("layer raster domain mismatch");
  const unsigned count = k == 1 ? 2 : k == 6 ? 3 : 1;
  if (schema.fields.size() != count)
    return bad("layer field membership mismatch");
  for (unsigned i = 0; i < count; ++i) {
    const char* key = k == 1   ? (i == 0 ? "coverage" : "emission")
                      : k == 2 ? "response"
                      : k == 3 ? "raw_sum"
                      : k == 4 ? "contributions"
                      : k == 5 ? "weighted"
                      : i == 0 ? "valid"
                      : i == 1 ? "coverage"
                               : "emission";
    const auto type = k == 4 || k == 5   ? ElementType::Float64
                      : k == 6 && i == 0 ? ElementType::UInt8
                                         : ElementType::Float32;
    const std::uint64_t width = k == 4 || k == 5   ? 8
                                : k == 1 && i == 1 ? 3
                                : k == 6           ? (i == 0   ? 1
                                                      : i == 1 ? 4
                                                               : 3)
                                                   : 4;
    auto rows = extent(ResultExtentKind::Fixed, spec.height * spec.width);
    if (k == 4 || (k == 6 && i == 1))
      rows = extent(ResultExtentKind::RuntimeCount);
    if (k == 6 && i == 2)
      rows = extent(ResultExtentKind::FieldRows, 1, 1);
    const auto& f = schema.fields[i];
    if (f.key != key || f.element_type != type || f.record_shape.size() != 1 ||
        f.record_shape[0] != width || !same_extent(f.rows, rows))
      return bad("layer field contract mismatch");
  }
  return Status::success();
}
Result<LayerSpec> layer_spec(const SchemaTemplate& schema) {
  if (!has_layer_schema(schema.id))
    return Result<LayerSpec>(bad("unrelated layer schema"));
  auto s = validate_layer_schema(schema);
  return s.ok() ? parse(schema) : Result<LayerSpec>(s);
}
Status validate_layer_result(
    const ResultRef& result, const ResourceBudget& resources,
    std::uint64_t maximum_window, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return environment_error();
  if (!result.valid() || !result.owned_by(resources))
    return bad("foreign layer result owner");
  auto s = result.schema().validate(true);
  if (!s.ok())
    return s;
  const auto k = kind(result.schema().id);
  if (!k)
    return Status::success();
  auto charged = [&](std::uint64_t work) {
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "layer validation cancelled"};
    auto status = resources.consume({work});
    return status.ok() && consume_work ? consume_work(work) : status;
  };
  s = charged(result.schema().canonical_size());
  if (!s.ok())
    return s;
  auto facts = result.descriptor();
  if (!facts.ok())
    return facts.status();
  const auto descriptor = facts.value();
  auto read = [&](unsigned field, std::uint64_t row, void* destination,
                  std::size_t bytes) {
    auto status = charged(1);
    if (!status.ok())
      return status;
    auto plan = result.prepare_read(descriptor, field, row, 1);
    if (!plan.ok())
      return plan.status();
    auto loaded = plan.value().load(maximum_window, cancellation);
    if (!loaded.ok())
      return loaded.status();
    if (loaded.value()->bytes().size() != bytes)
      return bad("layer row byte mismatch");
    std::memcpy(destination, loaded.value()->bytes().data(), bytes);
    return Status::success();
  };
  unsigned field = 0;
  std::uint64_t count = descriptor.rows(0);
  if (k == 6) {
    std::uint8_t valid = 0;
    s = read(0, 0, &valid, 1);
    if (!s.ok())
      return s;
    if (valid > 1 || descriptor.rows(1) != valid || descriptor.rows(2) != valid)
      return bad("optional Layer count differs from valid bit");
    count = valid;
    field = 1;
  }
  for (std::uint64_t row = 0; row < count; ++row) {
    if (k == 4 || k == 5) {
      WeightedLayerSum sum;
      s = read(0, row, sum.components.data(), 64);
      if (s.ok())
        s = k == 4 ? validate_layer_contribution({sum.components})
                   : validate_weighted_layer_sum(sum);
    } else {
      std::array<float, 4> values{};
      s = read(field, row, values.data(), 16);
      if (!s.ok())
        return s;
      std::array<float, 3> p{values[0], values[1], values[2]};
      if (k == 1 || k == 6) {
        LayerPixel pixel{{p, values[3]}, {}};
        s = read(field + 1, row, pixel.emission.data(), 12);
        if (s.ok())
          s = validate_layer(pixel);
      } else if (k == 2) {
        s = response_valid({p, values[3]});
      } else {
        s = validate_raw_rgba_sum({p, values[3]});
      }
    }
    if (!s.ok())
      return s;
  }
  return Status::success();
}
}  // namespace ps
