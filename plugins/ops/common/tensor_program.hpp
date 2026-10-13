#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/typed_need_validation.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"
#include "photospider/plugin/tile_helpers.hpp"

namespace ps::plugin_internal::tensor_ops {
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw result.status();
  return result.take_value();
}
inline void require(Status status) {
  if (!status.ok())
    throw status;
}
inline SchemaTemplate scalar_schema() {
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {1}};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
inline OperationPortConstraint port() {
  OperationPortConstraint constraint;
  constraint.kind = OperationPortKind::Result;
  constraint.result_schema_id = "photospider.tensor";
  constraint.result_schema_version = 1;
  constraint.tensor_key = "samples";
  return constraint;
}
inline Status check_tensor(const OperationMetadata& metadata) {
  if (!metadata.result_schema || metadata.result_schema->tensors.size() != 1 ||
      !metadata.result_schema->fields.empty())
    return {ErrorCode::TypeMismatch,
            "operation requires one tensor member and no scalar fields"};
  return metadata.result_schema->validate(true);
}
struct Identity final {
  bool whole = false;
  bool started = false;
  Footprint output;
  explicit Identity(bool whole = false) : whole(whole) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto checked_inputs = ops_validation::typed_inputs(phase);
    if (!checked_inputs.ok())
      return Result<ResultProgramPoll>(checked_inputs);
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& schema = *phase.query.output.result_schema;
    const auto& tensor = schema.tensors[0];
    const auto shape = tensor.sample_shape();
    if (!started) {
      output = phase.query.tensor_outputs &&
                       (!whole || phase.query.tensor_outputs->empty())
                   ? *phase.query.tensor_outputs
                   : take(Footprint::all(shape));
      started = true;
      if (!output.empty()) {
        ResultProgramNeed need;
        {
          auto tile_need_0 = sdk::tile_need_for_samples(
              phase.resources, phase.query, 0, 0, output);
          if (!tile_need_0.ok())
            return Result<ResultProgramPoll>(tile_need_0.status());
          need.tiles.push_back(tile_need_0.take_value());
        }
        return Result<ResultProgramPoll>(std::move(need));
      }
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        phase.query.tile_extent, phase.query.resources));
    std::vector<ResultViewAxis> axes(shape.size());
    for (std::size_t axis = 0; axis < axes.size(); ++axis)
      axes[axis].output_axis = static_cast<std::int32_t>(axis);
    for (const auto& box : output.boxes()) {
      const auto& input = phase.tensors->at({0, 0});
      auto window = take(input.acquire(box, phase.query.cancellation));
      ResultTensorViewTransform transform;
      transform.source_axes = axes;
      auto published = builder.publish_tensor_view(0, box, window, transform,
                                                   phase.query.cancellation);
      if (!published.ok() && tensor.layout.spatial &&
          published.reason == FailureReason::ViewUnavailable)
        published = builder.publish_tensor_view(0, box, {&window},
                                                phase.query.cancellation);
      if (!published.ok() &&
          published.reason == FailureReason::ViewUnavailable) {
        const auto width = Value::element_size(tensor.descriptor.element_type);
        published = builder.publish_tensor_kernel(
            0, box,
            [&](const auto& writers) {
              for (const auto& writer : writers) {
                const auto& dims = writer.region().dimensions();
                const auto sample_axis = writer.sample_axis();
                std::vector<std::uint64_t> at;
                for (auto d : dims)
                  at.push_back(d.offset);
                for (;;) {
                  auto work = phase.consume_work(1);
                  if (!work.ok())
                    return work;
                  auto source = window.row_run(at);
                  if (!source.ok())
                    return source.status();
                  auto destination = writer.row_run(at);
                  if (!destination.ok())
                    return destination.status();
                  const auto n = std::min(source.value().samples,
                                          destination.value().samples);
                  if (source.value().sample_stride_bytes ==
                          static_cast<std::int64_t>(width) &&
                      destination.value().sample_stride_bytes ==
                          static_cast<std::int64_t>(width)) {
                    std::memcpy(destination.value().data, source.value().data,
                                n * width);
                  } else {
                    for (std::uint64_t i = 0; i < n; ++i)
                      std::memcpy(
                          destination.value().data +
                              static_cast<std::ptrdiff_t>(
                                  static_cast<__int128>(i) *
                                  destination.value().sample_stride_bytes),
                          source.value().data +
                              static_cast<std::ptrdiff_t>(
                                  static_cast<__int128>(i) *
                                  source.value().sample_stride_bytes),
                          width);
                  }
                  at[sample_axis] += n;
                  if (at[sample_axis] <
                      dims[sample_axis].offset + dims[sample_axis].extent)
                    continue;
                  at[sample_axis] = dims[sample_axis].offset;
                  bool advanced = false;
                  for (std::size_t axis = dims.size(); axis;) {
                    --axis;
                    if (axis == sample_axis)
                      continue;
                    if (++at[axis] < dims[axis].offset + dims[axis].extent) {
                      advanced = true;
                      break;
                    }
                    at[axis] = dims[axis].offset;
                  }
                  if (!advanced)
                    break;
                }
              }
              return Status::success();
            },
            phase.query.cancellation);
      }
      require(published);
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const Status& status) {
    return Result<ResultProgramPoll>(status);
  }
};
struct Constant final {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto checked_inputs = ops_validation::typed_inputs(phase);
    if (!checked_inputs.ok())
      return Result<ResultProgramPoll>(checked_inputs);
    const auto& schema = *phase.query.output.result_schema;
    auto builder = take(ResultBuilder::start(phase.resources, schema,
                                             phase.query.semantic_key));
    const double number = std::get<double>(phase.query.parameters.at("value"));
    require(builder.publish_tensor_kernel(
        0, Region::whole({1}),
        [&](const auto& writers) {
          auto row = writers[0].row_run({0});
          if (!row.ok())
            return row.status();
          std::memcpy(row.value().data, &number, sizeof(number));
          return Status::success();
        },
        phase.query.cancellation));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const Status& status) {
    return Result<ResultProgramPoll>(status);
  }
};
inline OperationTraits traits(std::uint32_t inputs, std::uint64_t state,
                              std::uint32_t stages = 2) {
  OperationTraits traits;
  traits.input_count = inputs;
  traits.input_schema.assign(inputs, port());
  auto& output = traits.outputs[0];
  output.output_schema = port();
  output.result_schema = scalar_schema();
  output.region_rule = OperationRegionRule::Tiled;
  output.continuation_bytes = state;
  output.maximum_dependency_stages = stages;
  return traits;
}
inline OperationDefinition identity(const char* key, bool whole = false) {
  OperationDefinition operation;
  operation.key = key;
  operation.traits = traits(1, sizeof(Identity));
  if (whole)
    operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  OperationPortConstraint tensor;
  tensor.kind = OperationPortKind::Result;
  tensor.element_type_mask = 127;
  operation.traits.input_schema[0] = tensor;
  operation.traits.outputs[0].output_schema = tensor;
  operation.traits.requires_metadata_specialization = true;
  operation.traits.outputs[0].data_movement = DataMovementKind::BitwiseMapped;
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    auto valid = check_tensor(inputs[0]);
    if (!valid.ok())
      return Result<std::vector<OperationOutputSpecialization>>(valid);
    OperationOutputSpecialization output;
    output.metadata.result_schema = inputs[0].result_schema;
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.start_result = [whole](const ResultProgramQuery&,
                                   const BufferAllocator& allocator) {
    return ResultContinuation::make<Identity>(allocator, whole);
  };
  return operation;
}
}  // namespace ps::plugin_internal::tensor_ops
