#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "01-numeric/numeric_tensor_program.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class FiniteElementwise { Abs, Minimum, Maximum };
struct FiniteElementwiseProgram final {
  FiniteElementwise kind;
  unsigned stage = 0;
  explicit FiniteElementwiseProgram(FiniteElementwise kind) : kind(kind) {}
  template <class Number>
  Status write(const ResultProgramPhase& phase,
               const ResultTensorWriteWindow& writer,
               const FootprintLimits& limits) {
    auto left = math_take(phase.tensors->at({0, 0}).acquire(
        writer.region(), phase.query.cancellation));
    std::optional<ResultTensorReadWindow> right;
    if (kind != FiniteElementwise::Abs)
      right = math_take(phase.tensors->at({1, 0}).acquire(
          writer.region(), phase.query.cancellation));
    MathTensorWriter destination(writer);
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto samples =
        math_take(Footprint::from_regions(shape, {writer.region()}, limits));
    return samples.visit(
        [&](const auto& at) {
          auto status = phase.consume_work(1 + at.size() * (right ? 2 : 1));
          if (!status.ok())
            return status;
          auto a_run = left.row_run(at);
          if (!a_run.ok())
            return a_run.status();
          Number a, b = 0;
          std::memcpy(&a, a_run.value().data, sizeof(a));
          if (right) {
            auto b_run = right->row_run(at);
            if (!b_run.ok())
              return b_run.status();
            std::memcpy(&b, b_run.value().data, sizeof(b));
          }
          if (!std::isfinite(a) || !std::isfinite(b))
            return Status{ErrorCode::OperationFailed,
                          "nonfinite basic operation input or intermediate"};
          const Number number =
              kind == FiniteElementwise::Abs ? std::abs(a)
              : a == 0 && b == 0 ? (kind == FiniteElementwise::Minimum
                                        ? static_cast<Number>(-0.0)
                                        : static_cast<Number>(0.0))
              : kind == FiniteElementwise::Minimum ? std::min(a, b)
                                                   : std::max(a, b);
          std::memcpy(destination.address(at), &number, sizeof(number));
          return Status::success();
        },
        UINT64_MAX, phase.query.cancellation);
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto scratch =
        math_take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    FootprintLimits limits;
    limits.consume_work = phase.consume_work;
    limits.cancellation = phase.query.cancellation;
    const auto output = phase.query.tensor_outputs
                            ? *phase.query.tensor_outputs
                            : math_take(Footprint::all(shape, limits));
    const auto inputs = kind == FiniteElementwise::Abs ? 1U : 2U;
    if (stage < 2 && !output.empty()) {
      ResultProgramNeed need;
      bool expanded = false;
      for (uint32_t port = 0; port < inputs; ++port) {
        if (stage == 0) {
          const auto& spec = phase.query.inputs[port].result_schema->tensors[0];
          auto validation = math_take(spec.close_samples(output, limits));
          expanded = expanded || validation != output;
          need.tensors.push_back({port, 0, std::move(validation), 4});
        } else {
          need.tensors.push_back({port, 0, output, 1});
        }
      }
      if (stage == 0 && !expanded) {
        // A coincident validation/data footprint needs only one supply poll.
        for (auto& tensor : need.tensors)
          tensor.roles = 5;
        stage = 2;
      } else {
        ++stage;
      }
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = math_take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        phase.association ? std::vector<uint64_t>(phase.association->begin(),
                                                  phase.association->end())
                          : std::vector<uint64_t>{},
        phase.query.tile_height, phase.query.tile_width,
        phase.query.resources));
    ResultRelation basis, relation;
    // Empty has no input observations or payload reads. Static preparation has
    // already checked the complete schema envelope.
    if (output.empty()) {
      basis = math_take(ResultRelation::cartesian(phase.resources, 1, {}));
    } else {
      for (uint32_t port = 0; port < inputs; ++port) {
        auto descriptor = math_take(ResultRelation::cartesian(
            phase.resources, 1,
            {port, 8, 0, 1, ResultSupportTarget::Descriptor, 0}));
        basis = basis.valid() ? math_take(ResultRelation::unite(
                                    phase.resources, {basis, descriptor}))
                              : std::move(descriptor);
        std::vector<ResultMappedAxis> axes(shape.size());
        for (uint32_t axis = 0; axis < shape.size(); ++axis)
          axes[axis].output_axis = axis;
        auto data = math_take(ResultRelation::mapped(
            phase.resources, shape, Region::whole(shape), shape, axes,
            {port, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
        const auto& spec = phase.query.inputs[port].result_schema->tensors[0];
        auto tuple =
            input_internal::tuple_channel_axis(spec.descriptor, spec.facets);
        if (tuple) {
          const auto axis = spec.batch_axes.size() + *tuple;
          axes[axis].output_axis = -1;
          axes[axis].extent = shape[axis];
        }
        for (std::size_t cell =
                 spec.descriptor.shape.size() - spec.atomic_trailing_axes;
             cell < spec.descriptor.shape.size(); ++cell) {
          const auto axis = spec.batch_axes.size() + cell;
          axes[axis].output_axis = -1;
          axes[axis].extent = shape[axis];
        }
        auto validation = math_take(ResultRelation::mapped(
            phase.resources, shape, Region::whole(shape), shape, axes,
            {port, 4, 0, 0, ResultSupportTarget::Tensor, 0}));
        auto observed = math_take(
            ResultRelation::unite(phase.resources, {data, validation}));
        relation = relation.valid()
                       ? math_take(ResultRelation::unite(phase.resources,
                                                         {relation, observed}))
                       : std::move(observed);
      }
    }
    math_require(builder.bind_descriptor_relation(basis));
    if (!output.empty()) {
      input_internal::Float32Environment environment;
      if (!environment.active())
        return Result<ResultProgramPoll>(Status{
            ErrorCode::OperationFailed, "numeric environment unavailable"});
      for (const auto& box : output.boxes())
        math_require(builder.publish_tensor_kernel(
            0, box,
            [&](const auto& writers) {
              return math_callback(phase, [&] {
                for (const auto& writer : writers) {
                  auto status = schema.tensors[0].descriptor.element_type ==
                                        ElementType::Float32
                                    ? write<float>(phase, writer, limits)
                                    : write<double>(phase, writer, limits);
                  if (!status.ok())
                    return status;
                }
                return phase.consume_work(0);
              });
            },
            relation, {true, true, true, true}, phase.query.cancellation));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{math_take(builder.seal()), true});
  } catch (const Status& status) {
    return Result<ResultProgramPoll>(status);
  }
};
inline OperationDefinition finite_elementwise(const char* key,
                                              FiniteElementwise kind) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = kind == FiniteElementwise::Abs ? 1 : 2;
  traits.input_schema.resize(traits.input_count);
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.element_type_mask = 12;
  }
  set_whole_tensor_output(traits, ElementType::Float64,
                          sizeof(FiniteElementwiseProgram));
  traits.outputs[0].key = "value";
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  traits.outputs[0].maximum_dependency_stages = 3;
  traits.requires_metadata_specialization = true;
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    for (const auto& input : inputs)
      if (!input.result_schema || !input.result_schema->fields.empty() ||
          input.result_schema->tensors.size() != 1)
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "finite elementwise requires one tensor per Result"});
    const auto& first = inputs[0].result_schema->tensors[0];
    const auto shape = first.sample_shape();
    for (const auto& input : inputs) {
      const auto& tensor = input.result_schema->tensors[0];
      if (tensor.sample_shape() != shape ||
          tensor.descriptor.element_type != first.descriptor.element_type)
        return Answer(Status{ErrorCode::TypeMismatch,
                             "basic inputs must share shape and dtype"});
    }
    OperationOutputSpecialization output;
    output.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_tensor_schema(first.descriptor.element_type, shape));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.start_result = [kind](const auto&, const auto& allocator) {
    return ResultContinuation::make<FiniteElementwiseProgram>(allocator, kind);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
