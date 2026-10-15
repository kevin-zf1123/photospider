#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/tile_helpers.hpp"

namespace gpu_result {
struct Failure : std::runtime_error {
  ps::Status status;
  explicit Failure(ps::Status status)
      : std::runtime_error(status.message), status(std::move(status)) {}
};
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw Failure(result.status());
  return result.take_value();
}
inline void check(ps::Status status) {
  if (!status.ok())
    throw Failure(std::move(status));
}
inline ps::SchemaTemplate schema(ps::ElementType type, std::uint64_t count) {
  ps::SchemaTemplate value;
  value.id = "example.gpu.tensor";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, {count}};
  value.tensors.push_back(std::move(tensor));
  return value;
}
inline ps::WorkflowInputDeclaration declaration(
    std::uint64_t id, std::string name, const ps::SchemaTemplate& schema) {
  ps::WorkflowInputDeclaration input;
  input.id = id;
  input.name = std::move(name);
  input.result_schema = std::make_shared<const ps::SchemaTemplate>(schema);
  return input;
}
inline ps::OperationOutputTraits output(const ps::SchemaTemplate& schema) {
  ps::OperationOutputTraits output;
  output.key = "value";
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = std::string(schema.id);
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = schema;
  output.region_rule = ps::OperationRegionRule::Tiled;
  output.continuation_bytes = 1;
  output.maximum_dependency_stages = 4;
  return output;
}
inline ps::ResultBuilder builder(const ps::ResultProgramPhase& phase) {
  auto result = take(ps::ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_extent));
  return result;
}
inline ps::Result<ps::ResultProgramPoll> need(
    const ps::ResultProgramPhase& phase, const ps::Footprint& samples) {
  ps::ResultProgramNeed need;
  need.tiles.push_back(take(ps::sdk::tile_need_for_samples(
      phase.resources, phase.query, 0, 0, samples)));
  return ps::Result<ps::ResultProgramPoll>(std::move(need));
}
inline ps::Footprint demand(const ps::ResultProgramPhase& phase) {
  return phase.query.tensor_outputs.value_or(take(ps::Footprint::all(
      phase.query.output.result_schema->tensors[0].sample_shape())));
}
inline ps::Result<ps::ResultProgramPoll> view(
    const ps::ResultProgramPhase& phase) {
  auto result = builder(phase);
  ps::ResultTensorViewTransform transform;
  for (std::uint32_t axis = 0;
       axis <
       phase.query.output.result_schema->tensors[0].sample_shape().size();
       ++axis)
    transform.source_axes.push_back({static_cast<std::int32_t>(axis), 0, 1, 1});
  for (const auto& box : demand(phase).boxes()) {
    auto window =
        take(phase.tensors->at({0, 0}).acquire(box, phase.query.cancellation));
    check(result.publish_tensor_view(0, box, window, transform,
                                     phase.query.cancellation));
  }
  return ps::Result<ps::ResultProgramPoll>(
      ps::ResultPublication{take(result.seal()), true});
}
inline float number(const ps::ResultRef& result, std::uint64_t at) {
  float number = 0;
  check(result.read_tensor(take(result.descriptor()), 0, {at}, &number, 4));
  return number;
}
inline ps::Status gpu_status(const ps::ResultProgramPhase& phase, int code) {
  if (phase.gpu_status) {
    auto status = phase.gpu_status();
    if (!status.ok())
      return status;
  }
  return code ? ps::Status{ps::ErrorCode::OperationFailed, "native service"}
              : ps::Status::success();
}
}  // namespace gpu_result
