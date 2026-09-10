#include "plugin/color_operations.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "numeric_common.hpp"  // NOLINT(build/include_subdir)

namespace ps::plugin_internal {
namespace {
Result<OperationMetadata> metadata(const OperationTraits& traits,
                                   const OperationInvocation& call) {
  auto resolved =
      resolve_operation_traits(traits, call.inputs.size(), call.parameters);
  if (!resolved.ok())
    return Result<OperationMetadata>(resolved.status());
  std::vector<OperationMetadata> inputs;
  for (const auto& input : call.inputs)
    inputs.push_back({input.descriptor(), input.facets()});
  return infer_operation_output(resolved.value(), inputs, call.parameters);
}
SemanticDescriptor semantic(const Value& value) {
  for (const auto& facet : value.facets())
    if (facet.key == "photospider.image" || facet.key == "photospider.semantic")
      return decode_semantic(facet).take_value();
  return {};
}
Result<Value> publish(MutableValue output,
                      const std::vector<ValueFacet>& facets,
                      const CancellationToken& cancellation) {
  if (cancellation.cancelled()) {
    Status status;
    status.code = ErrorCode::Cancelled;
    return Result<Value>(status);
  }
  return std::move(output).publish(facets);
}
Result<Value> channels(const OperationInvocation& call,
                       const OperationTraits& traits) {
  auto inferred = metadata(traits, call);
  if (!inferred.ok())
    return Result<Value>(inferred.status());
  const auto& meta = inferred.value();
  auto made = MutableValue::allocate(meta.descriptor, call.output_region,
                                     call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto rule = traits.output_semantic_rule;
  const bool extract = rule == OperationSemanticRule::ExtractChannel;
  const bool merge = rule == OperationSemanticRule::MergeChannelsParameter;
  std::vector<std::uint32_t> indices;
  if (extract)
    indices.push_back(static_cast<std::uint32_t>(
        std::get<std::int64_t>(call.parameters.at("index"))));
  else if (!merge)
    indices = channel_indices_from_parameter(
                  std::get<std::string>(call.parameters.at("indices")))
                  .take_value();
  const auto width = Value::element_size(meta.descriptor.element_type);
  const auto count = extract ? 1 : meta.descriptor.shape[2];
  std::uint64_t destination = 0;
  for (std::uint64_t y = 0; y < meta.descriptor.shape[0]; ++y) {
    for (std::uint64_t x = 0; x < meta.descriptor.shape[1]; ++x) {
      if (((y * meta.descriptor.shape[1] + x) & 1023U) == 0 &&
          call.cancellation.cancelled())
        return publish(std::move(output), meta.facets, call.cancellation);
      for (std::uint64_t c = 0; c < count; ++c) {
        const auto& input = call.inputs[merge ? c : 0];
        const auto coordinate =
            merge ? std::vector<std::uint64_t>{y, x}
                  : std::vector<std::uint64_t>{y, x, indices[c]};
        std::memcpy(
            output.data() + destination,
            input.bytes().data() + input.byte_address(coordinate).value(),
            width);
        destination += width;
      }
    }
  }
  return publish(std::move(output), meta.facets, call.cancellation);
}
// Rational sRGB/D65 matrices and CIELAB thresholds: W3C CSS Color 4
// https://www.w3.org/TR/css-color-4/#color-conversion-code
// Reference white is always supplied by the typed descriptor; no adaptation.
std::array<double, 3> transform_color(const std::array<double, 3>& input,
                                      const SemanticDescriptor& source,
                                      OperationSemanticRule rule) {
  constexpr double rgb_xyz[3][3] = {
      {506752. / 1228815, 87881. / 245763, 12673. / 70218},
      {87098. / 409605, 175762. / 245763, 12673. / 175545},
      {7918. / 409605, 87881. / 737289, 1001167. / 1053270}};
  constexpr double xyz_rgb[3][3] = {
      {12831. / 3959, -329. / 214, -1974. / 3959},
      {-851781. / 878810, 1648619. / 878810, 36519. / 878810},
      {705. / 12673, -2585. / 12673, 705. / 667}};
  constexpr double epsilon = 216. / 24389, kappa = 24389. / 27;
  std::array<double, 3> result{};
  if (rule == OperationSemanticRule::RgbToXyz ||
      rule == OperationSemanticRule::XyzToRgb) {
    const auto& matrix =
        rule == OperationSemanticRule::RgbToXyz ? rgb_xyz : xyz_rgb;
    for (std::size_t row = 0; row < 3; ++row)
      for (std::size_t col = 0; col < 3; ++col)
        result[row] += matrix[row][col] * input[col];
  } else if (rule == OperationSemanticRule::XyzToLab) {
    std::array<double, 3> f{};
    for (std::size_t c = 0; c < 3; ++c) {
      const double relative = input[c] / source.white[c];
      f[c] = relative > epsilon ? std::cbrt(relative)
                                : (kappa * relative + 16) / 116;
    }
    result = {116 * f[1] - 16, 500 * (f[0] - f[1]), 200 * (f[1] - f[2])};
  } else {
    const double y = (input[0] + 16) / 116;
    const double f[3] = {y + input[1] / 500, y, y - input[2] / 200};
    for (std::size_t c = 0; c < 3; ++c) {
      const double cube = f[c] * f[c] * f[c];
      const double relative = c == 1 && input[0] <= kappa * epsilon
                                  ? input[0] / kappa
                              : cube > epsilon ? cube
                                               : (116 * f[c] - 16) / kappa;
      result[c] = relative * source.white[c];
    }
  }
  return result;
}
Result<Value> colors(const OperationInvocation& call,
                     const OperationTraits& traits) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(Status::failure(
        ErrorCode::OperationFailed, "color numeric environment unavailable"));
  auto inferred = metadata(traits, call);
  if (!inferred.ok())
    return Result<Value>(inferred.status());
  const auto& meta = inferred.value();
  auto made = MutableValue::allocate(meta.descriptor, call.output_region,
                                     call.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto& input = call.inputs[0];
  if (traits.output_semantic_rule == OperationSemanticRule::Parameter) {
    auto status = numeric_internal::visit(
        input, call.cancellation, [&](auto index, const auto& coordinate) {
          std::memcpy(
              output.data() + index * 4,
              input.bytes().data() + input.byte_address(coordinate).value(), 4);
          return Status::success();
        });
    if (!status.ok())
      return Result<Value>(status);
    return publish(std::move(output), meta.facets, call.cancellation);
  }
  const auto source = semantic(input);
  const auto rule = traits.output_semantic_rule;
  const bool associate = rule == OperationSemanticRule::AssociateAlpha;
  const bool alpha_only =
      associate || rule == OperationSemanticRule::UnassociateAlpha;
  const auto count = meta.descriptor.shape[2];
  std::array<std::size_t, 3> order{0, 1, 2};
  if (!alpha_only) {
    const char* roles[3] = {"red", "green", "blue"};
    if (source.model == "xyz") {
      roles[0] = "x";
      roles[1] = "y";
      roles[2] = "z";
    } else if (source.model == "lab") {
      roles[0] = "lightness";
      roles[1] = "a";
      roles[2] = "b";
    }
    for (std::size_t c = 0; c < 3; ++c)
      for (std::size_t i = 0; i < 3; ++i)
        if (source.channels[i].role == roles[c])
          order[c] = i;
  }
  for (std::uint64_t y = 0; y < meta.descriptor.shape[0]; ++y) {
    for (std::uint64_t x = 0; x < meta.descriptor.shape[1]; ++x) {
      const auto pixel = y * meta.descriptor.shape[1] + x;
      if ((pixel & 1023U) == 0 && call.cancellation.cancelled())
        return publish(std::move(output), meta.facets, call.cancellation);
      std::array<double, 3> numbers{};
      for (std::size_t c = 0; c < 3; ++c)
        numbers[c] = numeric_internal::read<float>(input, {y, x, order[c]});
      if (alpha_only) {
        const double alpha = numeric_internal::read<float>(input, {y, x, 3});
        for (auto& number : numbers)
          number = alpha == 0 ? 0 : associate ? number * alpha : number / alpha;
      } else {
        numbers = transform_color(numbers, source, rule);
      }
      for (std::size_t c = 0; c < 3; ++c) {
        if (!std::isfinite(numbers[c]) ||
            std::abs(numbers[c]) > std::numeric_limits<float>::max())
          return Result<Value>(numeric_internal::numeric_failure(
              pixel, "color result outside finite Float32"));
        const float number = static_cast<float>(numbers[c]);
        std::memcpy(output.data() + (pixel * count + c) * 4, &number, 4);
      }
      if (count == 4)
        std::memcpy(
            output.data() + (pixel * count + 3) * 4,
            input.bytes().data() + input.byte_address({y, x, 3}).value(), 4);
    }
  }
  return publish(std::move(output), meta.facets, call.cancellation);
}
}  // namespace
Status register_color_operations(OperationRegistry* registry) {
  const char* keys[] = {"channel.extract",   "channel.swizzle",
                        "channel.merge",     "alpha.associate",
                        "alpha.unassociate", "color.rgb_to_xyz",
                        "color.xyz_to_rgb",  "color.xyz_to_lab",
                        "color.lab_to_xyz",  "color.assign"};
  const OperationSemanticRule rules[] = {
      OperationSemanticRule::ExtractChannel,
      OperationSemanticRule::SwizzleChannels,
      OperationSemanticRule::MergeChannelsParameter,
      OperationSemanticRule::AssociateAlpha,
      OperationSemanticRule::UnassociateAlpha,
      OperationSemanticRule::RgbToXyz,
      OperationSemanticRule::XyzToRgb,
      OperationSemanticRule::XyzToLab,
      OperationSemanticRule::LabToXyz,
      OperationSemanticRule::Parameter};
  for (std::size_t i = 0; i < 10; ++i) {
    OperationDefinition operation;
    operation.key = keys[i];
    auto& t = operation.traits;
    t.output_semantic_rule = rules[i];
    t.input_count = 1;
    t.input_schema.resize(1);
    auto& port = t.input_schema[0];
    port.kind = OperationPortKind::Typed;
    port.rank = 3;
    port.element_type_mask = 12;
    t.output_schema.kind = OperationPortKind::Typed;
    t.output_dtype_rule = OperationDtypeRule::Input;
    t.shape_rule = OperationShapeRule::PreserveFirstInput;
    t.requires_dense_output = true;
    if (i < 3) {
      t.shape_rule = OperationShapeRule::Axes;
      t.output_axes = {{OperationExtentSource::InputAxis, 1, {}, 0, 0, 0},
                       {OperationExtentSource::InputAxis, 1, {}, 0, 1, 0}};
      if (i == 0) {
        t.output_semantic_parameter = "index";
        t.parameter_schema = {
            {"index", OperationParameterType::Int64, true, true, 0, 63}};
      } else if (i == 1) {
        port.kind = OperationPortKind::Value;
        t.output_schema.kind = OperationPortKind::Value;
        t.output_semantic_parameter = "indices";
        t.parameter_schema = {
            {"indices", OperationParameterType::String, true}};
        t.output_axes.push_back(
            {OperationExtentSource::IndexListCount, 1, "indices", 0, 0, 0});
      } else {
        port.kind = OperationPortKind::Value;
        port.rank = 2;
        t.input_count = 0;
        t.repeated_minimum = 2;
        t.repeated_maximum = 4;
        t.output_semantic_parameter = "semantic";
        t.parameter_schema = {
            {"semantic", OperationParameterType::String, true}};
        t.output_axes.push_back(
            {OperationExtentSource::InputCount, 1, {}, 0, 0, 0});
      }
    } else {
      port.element_type_mask = 0;
      port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
      port.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
      t.output_schema.semantic_kind =
          static_cast<std::uint32_t>(SemanticKind::Image);
      if (i == 9) {
        port.kind = OperationPortKind::Value;
        port.semantic_kind = 0;
        t.output_semantic_parameter = "semantic";
        t.parameter_schema = {
            {"semantic", OperationParameterType::String, true}};
      }
    }
    operation.callback = [traits = t,
                          channel = i < 3](const OperationInvocation& call) {
      return channel ? channels(call, traits) : colors(call, traits);
    };
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
