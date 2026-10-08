#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace scheduler_result {
struct Failure : std::runtime_error {
  ps::Status status;
  explicit Failure(ps::Status failure)
      : std::runtime_error(failure.message), status(std::move(failure)) {}
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
inline ps::SchemaTemplate schema(ps::ElementType type,
                                 std::vector<std::uint64_t> shape) {
  ps::SchemaTemplate result;
  result.id = "benchmark.tensor";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, std::move(shape)};
  result.tensors.push_back(std::move(tensor));
  return result;
}
inline ps::ResultRef source(const ps::ResourceBudget& root,
                            const ps::SchemaTemplate& schema,
                            ps::ByteView bytes, unsigned tile = 128) {
  auto builder = take(ps::ResultBuilder::start(root, schema, "benchmark.input",
                                               {}, {}, tile, tile));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  check(builder.publish_tensor(
      0, ps::Region::whole(schema.tensors[0].sample_shape()), bytes,
      take(ps::ResultRelation::cartesian(
          root, take(schema.tensors[0].sample_count()), {})),
      {true, true, true, true}));
  return take(builder.seal());
}
inline ps::WorkflowInputDeclaration declaration(const ps::ResultRef& input) {
  ps::WorkflowInputDeclaration result;
  result.id = 1;
  result.name = "input";
  result.result_schema =
      std::make_shared<const ps::SchemaTemplate>(input.schema());
  return result;
}
inline std::vector<std::uint8_t> bytes(const ps::ResultRef& object) {
  const auto facts = take(object.descriptor());
  const auto& spec = object.schema().tensors[0];
  const auto full = take(ps::Footprint::all(spec.sample_shape()));
  if (facts.tensor_coverage(0) != full)
    throw std::runtime_error("incomplete benchmark output");
  const auto count = take(spec.sample_count());
  const auto width = ps::Value::element_size(spec.descriptor.element_type);
  if (count > SIZE_MAX / width)
    throw std::runtime_error("benchmark output size overflow");
  std::vector<std::uint8_t> result(count * width);
  auto window = take(
      object.acquire_tensor(facts, 0, ps::Region::whole(spec.sample_shape())));
  std::uint64_t offset = 0;
  check(full.visit(
      [&](const auto& at) {
        const auto row = window.row_run(at);
        if (!row.ok())
          return row.status();
        std::memcpy(result.data() + offset, row.value().data, width);
        offset += width;
        return ps::Status::success();
      },
      count));
  return result;
}
struct Program {
  unsigned inputs;
  bool increment, requested = false;
  Program(unsigned inputs, bool increment)
      : inputs(inputs), increment(increment) {}
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using namespace ps;  // NOLINT(build/namespaces)
    using Answer = Result<ResultProgramPoll>;
    // Fixed scratch covers the small standard-container temporaries below.
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto shape = spec.sample_shape();
    const bool empty =
        phase.query.tensor_outputs && phase.query.tensor_outputs->empty();
    if (!requested && !empty) {
      ResultProgramNeed need;
      for (unsigned port = 0; port < inputs; ++port)
        need.tensors.push_back({port, 0, take(Footprint::all(shape)), 13});
      requested = true;
      return Answer(std::move(need));
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        phase.query.tile_height, phase.query.tile_width,
        phase.query.resources));
    if (empty) {
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(phase.resources, 1, {}))));
      return Answer(ResultPublication{take(builder.seal()), true});
    }
    const auto count = take(spec.sample_count());
    if (count > UINT64_MAX / (shape.size() + inputs * 8))
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    check(phase.consume_work(count * (shape.size() + inputs * 8)));
    std::vector<ResultRelation> descriptors, supports;
    std::array<ResultTensorReadWindow, 3> windows;
    for (unsigned port = 0; port < inputs; ++port) {
      descriptors.push_back(take(ResultRelation::cartesian(
          phase.resources, 1,
          {port, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
      supports.push_back(take(ResultRelation::cartesian(
          phase.resources, count,
          {port, 5, 0, count, ResultSupportTarget::Tensor, 0},
          DependencyGuarantee::Conservative)));
      windows[port] = take(phase.tensors->at({port, 0}).acquire(
          Region::whole(shape), phase.query.cancellation));
    }
    check(builder.bind_descriptor_relation(
        take(ResultRelation::unite(phase.resources, descriptors))));
    auto support = take(ResultRelation::unite(phase.resources, supports));
    if (increment) {
      const auto row = take(windows[0].row_run({0}));
      std::int64_t number;
      std::memcpy(&number, row.data, sizeof(number));
      if (number == INT64_MAX)
        return Answer(Status{ErrorCode::OperationFailed, "increment overflow"});
      ++number;
      check(builder.publish_tensor(
          0, Region::whole(shape),
          ByteView(reinterpret_cast<const std::uint8_t*>(&number),
                   sizeof(number)),
          std::move(support), {true, true, true, true}));
    } else {
      check(take(Footprint::all(shape))
                .visit(
                    [&](const auto& at) {
                      const auto first = take(windows[0].row_run(at));
                      for (unsigned port = 1; port < inputs; ++port) {
                        const auto other = take(windows[port].row_run(at));
                        if (std::memcmp(first.data, other.data, 8))
                          return Status{ErrorCode::OperationFailed,
                                        "independent branch bits differ"};
                      }
                      return Status::success();
                    },
                    count, phase.query.cancellation));
      ResultTensorViewTransform identity;
      for (unsigned axis = 0; axis < shape.size(); ++axis)
        identity.source_axes.push_back(
            {static_cast<std::int32_t>(axis), 0, 1, 1});
      check(builder.publish_tensor_view(
          0, Region::whole(shape), windows[0], identity, std::move(support),
          {true, true, true, true}, phase.query.cancellation));
    }
    return Answer(ResultPublication{take(builder.seal()), true});
  } catch (const Failure& failure) {
    return ps::Result<ps::ResultProgramPoll>(failure.status);
  }
};
inline ps::OperationDefinition operation(bool increment) {
  using namespace ps;  // NOLINT(build/namespaces)
  OperationDefinition result;
  result.key = increment ? "benchmark.increment" : "benchmark.verify_join";
  auto& traits = result.traits;
  traits.input_count = increment ? 1 : 3;
  traits.input_schema.resize(traits.input_count);
  const auto type = increment ? ElementType::Int64 : ElementType::Float64;
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.element_type = static_cast<std::uint32_t>(type);
  }
  traits.cacheable = false;
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.element_type = static_cast<std::uint32_t>(type);
  output.result_schema = schema(type, {1});
  output.dependency_version = 2;
  output.region_rule = OperationRegionRule::Whole;
  output.maximum_dependency_stages = 2;
  output.continuation_bytes = sizeof(Program);
  result.specialize_metadata =
      [increment, type](
          const auto& inputs,
          const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto& first = *inputs[0].result_schema;
    for (const auto& input : inputs) {
      const auto& schema = *input.result_schema;
      if (!schema.fields.empty() || schema.tensors.size() != 1 ||
          !schema.tensors[0].facets.empty() ||
          !schema.tensors[0].batch_axes.empty() ||
          schema.tensors[0].layout.spatial ||
          schema.tensors[0].descriptor.element_type != type ||
          schema.tensors[0].sample_shape() != first.tensors[0].sample_shape() ||
          (increment &&
           schema.tensors[0].sample_shape() != std::vector<std::uint64_t>{1}))
        return Answer(
            Status{ErrorCode::TypeMismatch, "benchmark tensor input"});
    }
    OperationOutputSpecialization output;
    auto schema = first;
    schema.publication = PublishPolicy::CompleteBundle;
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  result.start_result = [increment](const auto&, const auto& allocator) {
    return ResultContinuation::make<Program>(allocator, increment ? 1U : 3U,
                                             increment);
  };
  return result;
}
}  // namespace scheduler_result
