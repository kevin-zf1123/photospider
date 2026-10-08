#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_lut3d.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "01-numeric/uniform_axis.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
Status type_error(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
struct Lut3dProgram {
  ColorModel model;
  SequenceProfile profile;
  bool tetrahedral = false, clamp = false;
};
Result<OperationPreparation> prepare(
    bool tetrahedral, SequenceProfile profile,
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<OperationPreparation>;
  auto source = color_array_from_parameter(
      std::get<std::string>(parameters.at("input_color_description")));
  auto destination = color_array_from_parameter(
      std::get<std::string>(parameters.at("output_color_description")));
  if (!source.ok())
    return Answer(source.status());
  if (!destination.ok())
    return Answer(destination.status());
  const auto& a = source.value();
  const auto& b = destination.value();
  if (a.model != b.model || a.model == ColorModel::Cmyk ||
      a.association != ColorAssociation::None ||
      b.association != ColorAssociation::None ||
      a.source_layout != ColorSourceLayout::Interleaved ||
      b.source_layout != ColorSourceLayout::Interleaved)
    return Answer(numeric_ops::array_parameter_error(
        "LUT3D requires same-model three-component interleaved colors"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& domain = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (domain != "clamp" && domain != "reject"))
    return Answer(
        numeric_ops::array_parameter_error("invalid LUT3D dtype/domain"));
  if (inputs.size() != 3)
    return Answer(type_error("LUT3D requires input, table, axis"));
  for (unsigned port = 0; port < 3; ++port) {
    const auto& tensor = inputs[port].result_schema->tensors[0];
    const ValueDescriptor descriptor{tensor.descriptor.element_type,
                                     tensor.sample_shape()};
    if ((descriptor.element_type != ElementType::Float64 &&
         (port == 2 || descriptor.element_type != ElementType::Float32)) ||
        (port == 0 &&
         (descriptor.shape.size() < 2 || descriptor.shape.size() > 8 ||
          descriptor.shape.back() != 3)) ||
        (port == 1 &&
         (descriptor.shape.size() != 4 || descriptor.shape.back() != 3)) ||
        (port == 2 && descriptor.shape != std::vector<std::uint64_t>{3, 3}))
      return Answer(type_error("invalid LUT3D port shape/dtype"));
    std::uint64_t count = 1;
    for (auto extent : descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(numeric_ops::array_parameter_error(
            "LUT3D logical product exceeds 2^40"));
      count *= extent;
    }
  }
  for (unsigned axis = 0; axis < 3; ++axis)
    if (inputs[1].result_schema->tensors[0].sample_shape()[axis] < 2 ||
        inputs[1].result_schema->tensors[0].sample_shape()[axis] > 256)
      return Answer(numeric_ops::array_parameter_error(
          "LUT3D axis extent must be 2..256"));
  auto input_facet = encode_color_array(a),
       output_facet = encode_color_array(b);
  if (!input_facet.ok())
    return Answer(input_facet.status());
  if (!output_facet.ok())
    return Answer(output_facet.status());
  for (unsigned port = 0; port < 2; ++port) {
    const auto& expected = port ? output_facet.value() : input_facet.value();
    for (const auto& facet : inputs[port].result_schema->tensors[0].facets)
      if (facet.key == "photospider.color-array" &&
          (facet.version != expected.version ||
           facet.payload != expected.payload))
        return Answer(type_error(
            "attached LUT3D colors disagree with static description"));
  }
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  OperationPreparation prepared;
  prepared.state = std::make_shared<Lut3dProgram>(
      Lut3dProgram{a.model, profile, tetrahedral, domain == "clamp"});
  prepared.outputs.resize(1);
  auto& output = prepared.outputs[0];
  auto schema = numeric_ops::numeric_tensor_schema(
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
      inputs[0].result_schema->tensors[0].sample_shape());
  schema.tensors[0].facets = {output_facet.take_value()};
  schema.tensors[0].atomic_trailing_axes = 1;
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  return Answer(std::move(prepared));
}
struct Lut3dPoint {
  std::array<std::uint64_t, 3> query{};
  std::array<unsigned, 3> cell{};
  numeric_ops::ExactLut3d::Support support;
};
struct Lut3dState {
  const Lut3dProgram* program;
  std::array<numeric_ops::UniformAxis, 3> axes;
  numeric_ops::ExactLut3d arithmetic;
  const ResultProgramPhase& phase;
  std::array<std::optional<numeric_ops::MathTensorReader>, 3> readers;
  std::function<Status(std::uint64_t)> consume;
  std::vector<std::uint64_t> shape, position;
  Lut3dState(const Lut3dProgram* value, const ResultProgramPhase& invocation)
      : program(value),
        axes{numeric_ops::UniformAxis(value->profile),
             numeric_ops::UniformAxis(value->profile),
             numeric_ops::UniformAxis(value->profile)},
        arithmetic(value->profile),
        phase(invocation),
        consume([this](auto amount) { return work(amount); }),
        shape(invocation.query.inputs[0]
                  .result_schema->tensors[0]
                  .sample_shape()),
        position(shape.size() - 1, 0) {
    for (auto& axis : axes)
      axis.knots = ResourceVector<std::uint64_t>(
          ResourceAllocator<std::uint64_t>(phase.resources));
    for (unsigned port = 0; port < readers.size(); ++port)
      readers[port].emplace(phase.tensors->at({port, 0}),
                            phase.query.cancellation);
  }
  Status work(std::uint64_t amount) const { return phase.consume_work(amount); }
  void advance() {
    for (auto i = position.size(); i; --i) {
      if (++position[i - 1] < shape[i - 1])
        break;
      position[i - 1] = 0;
    }
  }
  Status failure(const std::string& message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    return {ErrorCode::OperationFailed,
            message,
            reason,
            {FailureOrigin::Domain, FailureScope::Run}};
  }
  Result<std::uint64_t> read(unsigned port,
                             const std::vector<std::uint64_t>& at) {
    auto charged = work(at.size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const auto& input = phase.tensors->at({port, 0});
    const bool narrow =
        input.spec().descriptor.element_type == ElementType::Float32;
    auto bits = readers[port]->bits(at);
    const auto parts = BinaryParts::decode(bits, narrow);
    if (parts.nan || parts.infinite)
      return Result<std::uint64_t>(
          failure("nonfinite LUT3D input; port=" + std::to_string(port)));
    if (narrow) {
      const auto sign = (bits >> 31) << 63;
      if (!parts.magnitude) {
        bits = sign;
      } else {
        const auto top = 63 - __builtin_clzll(parts.significand);
        bits = sign |
               (static_cast<std::uint64_t>(parts.exponent + top + 1023) << 52) |
               ((parts.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
      }
    }
    return Result<std::uint64_t>(bits);
  }
  Status validate_color(const std::array<std::uint64_t, 3>& color,
                        unsigned port) const {
    if (program->model == ColorModel::Cielch ||
        program->model == ColorModel::Oklch) {
      const auto chroma = BinaryParts::decode(color[1], false);
      if (chroma.negative && chroma.magnitude)
        return failure("negative LUT3D chroma; port=" + std::to_string(port));
    }
    return Status::success();
  }
  std::vector<std::uint64_t> vertex(const Lut3dPoint& point,
                                    unsigned index) const {
    const auto bits = point.support.vertices[index];
    return {point.cell[0] + (bits & 1), point.cell[1] + ((bits >> 1) & 1),
            point.cell[2] + ((bits >> 2) & 1)};
  }
  numeric_ops::ExactLut3d::Cell cell(const Lut3dPoint& point) const {
    numeric_ops::ExactLut3d::Cell result;
    for (unsigned i = 0; i < 3; ++i)
      result[i] = {axes[i].knots[point.cell[i]],
                   axes[i].knots[point.cell[i] + 1]};
    return result;
  }
  Status classify(Lut3dPoint* point, bool weights = true) {
    auto at = position;
    at.push_back(0);
    // Validate every original component before any domain clamp can hide it.
    for (unsigned i = 0; i < 3; ++i) {
      at.back() = i;
      auto read_value = read(0, at);
      if (!read_value.ok())
        return read_value.status();
      point->query[i] = read_value.value();
    }
    auto valid = validate_color(point->query, 0);
    if (!valid.ok())
      return valid;
    for (unsigned i = 0; i < 3; ++i) {
      const auto& axis = axes[i];
      const auto key = axis.key(point->query[i]);
      unsigned lo = 0, hi = axis.knots.size();
      while (lo < hi) {
        auto work = consume(1);
        if (!work.ok())
          return work;
        const auto mid = lo + (hi - lo) / 2;
        if (axis.key(axis.knots[mid]) < key)
          lo = mid + 1;
        else
          hi = mid;
      }
      const auto size = static_cast<unsigned>(axis.knots.size());
      if (lo < size && axis.key(axis.knots[lo]) == key) {
        point->cell[i] = std::min(lo, size - 2);
      } else if (!lo || lo == size) {
        if (!program->clamp)
          return failure("LUT3D query outside axis=" + std::to_string(i));
        point->cell[i] = lo ? size - 2 : 0;
        point->query[i] = axis.knots[lo ? size - 1 : 0];
      } else {
        point->cell[i] = lo - 1;
      }
    }
    if (!weights)
      return Status::success();
    auto support = arithmetic.support(cell(*point), point->query,
                                      program->tetrahedral, consume);
    if (!support.ok())
      return support.status();
    point->support = support.take_value();
    return Status::success();
  }
  Status execute(const ResourceVector<ResultTensorWriteWindow>& writers) {
    for (unsigned axis = 0; axis < 3; ++axis) {
      std::array<std::uint64_t, 3> values{};
      for (unsigned j = 0; j < 3; ++j) {
        auto value = read(2, {axis, j});
        if (!value.ok())
          return value.status();
        values[j] = value.value();
      }
      auto status = axes[axis].validate(
          values,
          phase.query.inputs[1].result_schema->tensors[0].sample_shape()[axis],
          consume);
      if (!status.ok())
        return status.code == ErrorCode::OperationFailed
                   ? failure(status.message, status.reason)
                   : status;
    }
    const auto count =
        phase.query.output.result_schema->tensors[0].sample_count().value() / 3;
    // Preserve complete query/domain validation before selected table
    // arithmetic.
    for (std::uint64_t i = 0; i < count; ++i, advance()) {
      Lut3dPoint point;
      auto status = classify(&point, false);
      if (!status.ok())
        return status;
    }
    numeric_ops::MathTensorWriter output(writers[0]);
    const bool narrow =
        phase.query.output.result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    for (std::uint64_t row = 0; row < count; ++row, advance()) {
      Lut3dPoint point;
      auto status = classify(&point);
      if (!status.ok())
        return status;
      std::array<std::array<std::uint64_t, 3>, 8> colors{};
      for (unsigned i = 0; i < point.support.count; ++i) {
        auto at = vertex(point, i);
        at.push_back(0);
        for (unsigned channel = 0; channel < 3; ++channel) {
          at.back() = channel;
          auto value = read(1, at);
          if (!value.ok())
            return value.status();
          colors[i][channel] = value.value();
        }
        auto valid = validate_color(colors[i], 1);
        if (!valid.ok())
          return valid;
      }
      auto values =
          arithmetic.evaluate(cell(point), point.query, program->tetrahedral,
                              colors, narrow, consume);
      if (!values.ok())
        return values.status();
      auto output_at = position;
      output_at.push_back(0);
      for (unsigned channel = 0; channel < 3; ++channel) {
        output_at.back() = channel;
        if (BinaryParts::decode(values.value()[channel], narrow).infinite)
          return failure("LUT3D output overflow",
                         FailureReason::ArithmeticOverflow);
        std::memcpy(output.address(output_at), &values.value()[channel], width);
      }
    }
    return work(1);
  }
};
struct Lut3dKernel final {
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1 || !phase.query.prepared ||
        !phase.query.prepared->state())
      return {ErrorCode::OperationFailed,
              "LUT3D requires a prepared packed writer"};
    auto allocated = phase.allocator.allocate(sizeof(Lut3dState));
    if (!allocated.ok())
      return allocated.status();
    auto buffer = allocated.take_value();
    std::unique_ptr<Lut3dState, void (*)(Lut3dState*)> state(
        new (buffer.data()) Lut3dState(
            static_cast<const Lut3dProgram*>(phase.query.prepared->state()),
            phase),
        [](auto* value) { value->~Lut3dState(); });
    return state->execute(writers);
  }
};
using Lut3dResultProgram = numeric_ops::WholeTensorProgram<Lut3dKernel>;

OperationDefinition operation(const std::string& key, bool tetrahedral,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = key;
  auto& traits = result.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema)
    input.kind = OperationPortKind::Result;
  traits.input_schema[0].element_type_mask = 12;
  traits.input_schema[1].element_type_mask = 12;
  traits.input_schema[2].element_type_mask = 4;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"input_color_description", OperationParameterType::String},
      {"output_color_description", OperationParameterType::String},
      {"dtype", OperationParameterType::String},
      {"out_of_domain", OperationParameterType::String}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(Lut3dResultProgram));
  traits.workspace_bytes = sizeof(Lut3dState);
  result.prepare_static = [tetrahedral, profile](const auto& inputs,
                                                 const auto& parameters) {
    return prepare(tetrahedral, profile, inputs, parameters);
  };
  result.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Lut3dResultProgram>(allocator);
  };
  return result;
}
}  // namespace
Status register_lut3d_application(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (bool tetrahedral : {false, true}) {
      auto status = registry->register_operation(operation(
          std::string("curve.apply_lut3d_") +
              (tetrahedral ? "tetrahedral" : "trilinear") + profile.first,
          tetrahedral, profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
