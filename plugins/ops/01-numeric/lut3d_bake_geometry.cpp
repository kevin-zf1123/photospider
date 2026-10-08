#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/lut3d_bake_common.hpp"
#include "01-numeric/uniform_axis.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace bake_ops;  // NOLINT(build/namespaces)
enum class Geometry { Axis, Grid, Points, Extra, Color };
Result<std::vector<OperationOutputSpecialization>> specialize(
    Geometry kind, SequenceProfile profile,
    const std::vector<OperationMetadata>& inputs,
    const Parameters& parameters) {
  using Answer = Result<std::vector<OperationOutputSpecialization>>;
  if (inputs.size() != (kind == Geometry::Extra ? 2U : 1U))
    return Answer(mismatch("bake geometry inputs"));
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  auto color = color_array_from_parameter(std::get<std::string>(
      parameters.at(kind == Geometry::Color ? "color_description"
                                            : "input_color_description")));
  if (!color.ok())
    return Answer(color.status());
  if (color.value().model == ColorModel::Cmyk ||
      color.value().association != ColorAssociation::None ||
      color.value().source_layout != ColorSourceLayout::Interleaved)
    return Answer(invalid("bake geometry requires three-component colors"));
  for (const auto& input : inputs)
    if (!sole_tensor(input))
      return Answer(mismatch("bake geometry requires one Result tensor"));
  OperationOutputSpecialization output;
  if (kind == Geometry::Color) {
    auto checked = color_metadata(inputs[0], color.value());
    if (!checked.ok())
      return Answer(checked);
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype != "float32" && dtype != "float64")
      return Answer(invalid("bake color dtype"));
    output.metadata.descriptor = {
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
        tensor_descriptor(inputs[0]).shape};
  } else {
    if (tensor_descriptor(inputs[0]).element_type != ElementType::Float64 ||
        tensor_descriptor(inputs[0]).shape != std::vector<std::uint64_t>{3, 3})
      return Answer(mismatch("bake axis requires Float64[3,3]"));
    const auto dimensions = shape(parameters);
    output.metadata.descriptor.element_type = ElementType::Float64;
    if (kind == Geometry::Axis) {
      output.metadata.descriptor.shape = {3, 3};
    } else if (kind == Geometry::Grid) {
      output.metadata.descriptor.shape = {dimensions[0], dimensions[1],
                                          dimensions[2], 3};
    } else {
      const auto extras = std::get<std::int64_t>(parameters.at("extra_count"));
      if (extras < 0 || extras > 1048576 ||
          ((kind == Geometry::Extra) != (extras > 0)))
        return Answer(invalid("bake optional extra count"));
      if (kind == Geometry::Extra &&
          (tensor_descriptor(inputs[1]).element_type != ElementType::Float64 ||
           tensor_descriptor(inputs[1]).shape !=
               std::vector<std::uint64_t>{static_cast<std::uint64_t>(extras),
                                          3}))
        return Answer(mismatch("bake extras require Float64[M,3]"));
      output.metadata.descriptor.shape = {product(dimensions, 1) + extras, 3};
      if (kind == Geometry::Extra) {
        auto checked = color_metadata(inputs[1], color.value());
        if (!checked.ok())
          return Answer(checked);
      }
    }
  }
  if (kind != Geometry::Axis) {
    auto valid = validate_color_array_descriptor(color.value(),
                                                 output.metadata.descriptor);
    if (!valid.ok())
      return Answer(valid);
    auto facet = encode_color_array(color.value());
    if (!facet.ok())
      return Answer(facet.status());
    output.metadata.facets = {facet.take_value()};
    output.metadata.atomic_trailing_axes = 1;
  }
  auto schema = numeric_ops::numeric_tensor_schema(
      output.metadata.descriptor.element_type,
      output.metadata.descriptor.shape);
  schema.tensors[0].facets = std::move(output.metadata.facets);
  schema.tensors[0].atomic_trailing_axes = output.metadata.atomic_trailing_axes;
  output.metadata = {};
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  return Answer(std::vector<OperationOutputSpecialization>{std::move(output)});
}
struct GeometryProgram {
  Geometry kind;
  SequenceProfile profile;
  ColorModel model;
  std::array<std::uint64_t, 3> dimensions;
};
struct GeometryState {
  std::array<numeric_ops::UniformAxis, 3> axes;
  explicit GeometryState(SequenceProfile profile)
      : axes{numeric_ops::UniformAxis(profile),
             numeric_ops::UniformAxis(profile),
             numeric_ops::UniformAxis(profile)} {}
};
struct GeometryKernel {
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    auto status = write_values(phase, writers);
    if (status.detail.origin == FailureOrigin::Domain)
      status.detail.scope = FailureScope::Run;
    return status;
  }
  Status write_values(const ResultProgramPhase& phase,
                      const ResourceVector<ResultTensorWriteWindow>& writers) {
    const auto& program =
        *static_cast<const GeometryProgram*>(phase.query.prepared->state());
    const auto kind = program.kind;
    const auto dimensions = program.dimensions;
    const auto consume = phase.consume_work;
    auto scratch = phase.allocator.allocate(sizeof(GeometryState));
    if (!scratch.ok())
      return scratch.status();
    auto buffer = scratch.take_value();
    std::unique_ptr<GeometryState, void (*)(GeometryState*)> state(
        new (buffer.data()) GeometryState(program.profile),
        [](auto* value) { value->~GeometryState(); });
    auto& axes = state->axes;
    for (auto& axis : axes)
      axis.knots = ResourceVector<std::uint64_t>(
          ResourceAllocator<std::uint64_t>(phase.resources));
    std::array<std::optional<numeric_ops::MathTensorReader>, 2> readers;
    for (unsigned port = 0; port < phase.query.inputs.size(); ++port)
      readers[port].emplace(phase.tensors->at({port, 0}),
                            phase.query.cancellation);
    const auto read_value = [&](unsigned port, const auto& at) {
      auto charged = consume(at.size() + 1);
      if (!charged.ok())
        return Result<std::uint64_t>(charged);
      auto bits = readers[port]->bits(at);
      const bool narrow =
          tensor_descriptor(phase.query.inputs[port]).element_type ==
          ElementType::Float32;
      auto parts = BinaryParts::decode(bits, narrow);
      if (parts.nan || parts.infinite)
        return Result<std::uint64_t>(domain("nonfinite bake source color"));
      return Result<std::uint64_t>(promote(bits, narrow));
    };
    std::array<std::uint64_t, 9> axis_bits{};
    if (kind != Geometry::Color) {
      for (unsigned d = 0; d < 3; ++d) {
        std::array<std::uint64_t, 3> values{};
        for (unsigned j = 0; j < 3; ++j) {
          auto result = read_value(0, std::vector<std::uint64_t>{d, j});
          if (!result.ok())
            return (result.status());
          values[j] = axis_bits[3 * d + j] = result.value();
        }
        auto validated = axes[d].validate(values, dimensions[d], consume);
        if (!validated.ok())
          return (validated);
      }
    }
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const ValueDescriptor descriptor{tensor.descriptor.element_type,
                                     tensor.sample_shape()};
    const unsigned width = Value::element_size(descriptor.element_type);
    numeric_ops::MathTensorWriter writer(writers[0]);
    const auto at = Region::whole(descriptor.shape).dimensions();
    const auto count = tensor.sample_count().value();
    for (std::uint64_t offset = 0; offset < count;) {
      auto fuel = consume(64);
      if (!fuel.ok())
        return (fuel);
      auto residue = offset;
      std::vector<std::uint64_t> point(at.size());
      for (unsigned d = at.size(); d; --d) {
        point[d - 1] = at[d - 1].offset + residue % at[d - 1].extent;
        residue /= at[d - 1].extent;
      }
      if (kind == Geometry::Axis) {
        auto bits = axis_bits[point[0] * 3 + point[1]];
        std::memcpy(writer.address(point), &bits, 8);
        ++offset;
        continue;
      }
      std::array<std::uint64_t, 3> values{};
      for (unsigned c = 0; c < 3; ++c) {
        if (kind == Geometry::Grid) {
          values[c] = axes[c].knots[point[c]];
        } else if (kind == Geometry::Color) {
          point.back() = c;
          auto result = read_value(0, point);
          if (!result.ok())
            return (result.status());
          values[c] = result.value();
        } else if (point[0] < product(dimensions, 1)) {
          auto cell = point[0];
          std::array<unsigned, 3> index{};
          for (unsigned d = 3; d; --d) {
            index[d - 1] = cell % (dimensions[d - 1] - 1);
            cell /= dimensions[d - 1] - 1;
          }
          auto result = axes[c].sampling.weighted(
              axes[c].knots[index[c]], axes[c].knots[index[c] + 1], 1, 1, 2,
              false, false, consume);
          if (!result.ok())
            return (result.status());
          values[c] = result.value();
        } else {
          auto result = read_value(
              1,
              std::vector<std::uint64_t>{point[0] - product(dimensions, 1), c});
          if (!result.ok())
            return (result.status());
          values[c] = result.value();
          auto key = axes[c].key(values[c]);
          if (key < axes[c].key(axes[c].knots.front()) ||
              key > axes[c].key(axes[c].knots.back()))
            return (domain("extra validation point outside bake axis"));
        }
      }
      if (!model_valid(program.model, values))
        return (domain("model-invalid bake color"));
      for (unsigned c = 0; c < 3; ++c) {
        if (width == 4) {
          auto converted = axes[0].sampling.weighted(values[c], values[c], 1, 0,
                                                     1, true, false, consume);
          if (!converted.ok())
            return (converted.status());
          values[c] = converted.value();
          if (BinaryParts::decode(values[c], true).infinite)
            return (Status{ErrorCode::OperationFailed,
                           "bake source narrowing overflow",
                           FailureReason::ArithmeticOverflow,
                           {FailureOrigin::Domain, FailureScope::Group}});
        }
        point.back() = c;
        std::memcpy(writer.address(point), &values[c], width);
      }
      offset += 3;
    }
    return consume(1);
  }
};
using GeometryResultProgram = numeric_ops::WholeTensorProgram<GeometryKernel>;
OperationDefinition geometry(Geometry kind, SequenceProfile profile,
                             const std::string& key) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = kind == Geometry::Extra ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  traits.requires_metadata_specialization = true;
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = kind == Geometry::Color ? 12 : 4;
  }
  traits.parameter_schema =
      kind == Geometry::Color
          ? std::vector<
                OperationParameterSpec>{{"color_description",
                                         OperationParameterType::String},
                                        {"dtype",
                                         OperationParameterType::String}}
          : geometry_parameters();
  if (kind == Geometry::Points || kind == Geometry::Extra)
    traits.parameter_schema.push_back(
        {"extra_count", OperationParameterType::Int64, true, true, 0, 1048576});
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(GeometryResultProgram));
  traits.workspace_bytes = sizeof(GeometryState);
  definition.prepare_static =
      [kind, profile](const auto& inputs,
                      const auto& parameters) -> Result<OperationPreparation> {
    auto output = specialize(kind, profile, inputs, parameters);
    if (!output.ok())
      return Result<OperationPreparation>(output.status());
    auto color = color_array_from_parameter(std::get<std::string>(
        parameters.at(kind == Geometry::Color ? "color_description"
                                              : "input_color_description")));
    if (!color.ok())
      return Result<OperationPreparation>(color.status());
    OperationPreparation prepared;
    prepared.outputs = output.take_value();
    prepared.state = std::make_shared<const GeometryProgram>(
        GeometryProgram{kind, profile, color.value().model,
                        kind == Geometry::Color ? std::array<std::uint64_t, 3>{}
                                                : shape(parameters)});
    return Result<OperationPreparation>(std::move(prepared));
  };
  definition.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<GeometryResultProgram>(allocator);
  };
  return definition;
}
}  // namespace
Status register_lut3d_bake_geometry(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& item : {std::make_pair(Geometry::Axis, "axis"),
                             std::make_pair(Geometry::Grid, "grid"),
                             std::make_pair(Geometry::Points, "points"),
                             std::make_pair(Geometry::Extra, "points_extra"),
                             std::make_pair(Geometry::Color, "color")}) {
      auto status = registry->register_operation(geometry(
          item.first, profile.second,
          std::string("curve.bake_lut3d_") + item.second + profile.first));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
