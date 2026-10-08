#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace g4_result {
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
  ps::SchemaTemplate result;
  result.id = "example.g4.tensor";
  ps::ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {type, {count}};
  result.tensors.push_back(std::move(tensor));
  return result;
}
inline ps::WorkflowInputDeclaration declaration(
    std::uint64_t id, std::string name, const ps::SchemaTemplate& schema) {
  ps::WorkflowInputDeclaration result;
  result.id = id;
  result.name = std::move(name);
  result.result_schema = std::make_shared<const ps::SchemaTemplate>(schema);
  return result;
}
inline ps::ResultRef numbers(const ps::ResourceBudget& root,
                             const std::vector<double>& values) {
  auto shape = schema(ps::ElementType::Float64, values.size());
  auto builder = take(ps::ResultBuilder::start(root, shape, "g4.input"));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  check(builder.publish_tensor(
      0, ps::Region::whole({values.size()}),
      {reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * 8},
      take(ps::ResultRelation::cartesian(root, values.size(), {})),
      {true, true, true, true}));
  return take(builder.seal());
}
inline double number(const ps::ResultRef& value, std::uint64_t at = 0) {
  double result = 0;
  check(value.read_tensor(take(value.descriptor()), 0, {at}, &result, 8));
  return result;
}
inline ps::OperationOutputTraits output(const ps::SchemaTemplate& schema,
                                        std::uint64_t state_bytes,
                                        std::uint32_t stages = 2) {
  ps::OperationOutputTraits result;
  result.key = "value";
  result.output_schema.kind = ps::OperationPortKind::Result;
  result.output_schema.result_schema_id = std::string(schema.id);
  result.output_schema.result_schema_version = schema.version;
  result.result_schema = schema;
  result.region_rule = ps::OperationRegionRule::Dependency;
  result.dependency_version = 2;
  result.continuation_bytes = state_bytes;
  result.maximum_dependency_stages = stages;
  return result;
}
using Fill = std::function<ps::Status(
    const ps::ResultProgramQuery&, const ps::Region&, std::uint8_t*,
    std::uint64_t)>;  // NOLINT(whitespace/indent_namespace)
struct Source {
  std::shared_ptr<const Fill> fill;
  explicit Source(std::shared_ptr<const Fill> fill) : fill(std::move(fill)) {}
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    const auto& schema = *phase.query.output.result_schema;
    auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                                 phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
    auto demand = phase.query.tensor_outputs.value_or(
        take(ps::Footprint::all(schema.tensors[0].sample_shape())));
    for (const auto& box : demand.boxes()) {
      check(phase.query.cancellation.cancelled()
                ? ps::Status{ps::ErrorCode::Cancelled, {}}
                : ps::Status::success());
      const auto count = take(box.element_count());
      if (count > UINT64_MAX / 8)
        throw Failure(ps::Status{ps::ErrorCode::ResourceExhausted,
                                 "source byte extent overflow"});
      auto bytes = take(phase.resources.allocator().allocate(count * 8));
      check(phase.consume_work(count));
      check((*fill)(phase.query, box, bytes.data(), bytes.size()));
      check(builder.publish_tensor(
          0, box, ps::StridedLayout{0, {8}, {box.dimensions()[0].offset}},
          std::move(bytes).freeze(),
          take(ps::ResultRelation::cartesian(
              phase.resources, take(schema.tensors[0].sample_count()), {})),
          {true, true, true, true}));
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  } catch (const Failure& error) {
    return ps::Result<ps::ResultProgramPoll>(error.status);
  } catch (const std::bad_alloc&) {
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::ResourceExhausted, {}});
  } catch (const std::exception& error) {
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed, error.what()});
  }
};
inline ps::OperationDefinition source(std::string key,
                                      const ps::SchemaTemplate& schema,
                                      Fill fill) {
  if (schema.tensors.size() != 1 || !schema.fields.empty() ||
      schema.tensors[0].sample_shape().size() != 1 ||
      (schema.tensors[0].descriptor.element_type != ps::ElementType::Float64 &&
       schema.tensors[0].descriptor.element_type != ps::ElementType::Int64))
    throw Failure(ps::Status{ps::ErrorCode::TypeMismatch,
                             "G4 source requires one rank-one 64-bit tensor"});
  ps::OperationDefinition operation;
  operation.key = std::move(key);
  operation.traits.outputs = {output(schema, sizeof(Source), 1)};
  operation.traits.cacheable = false;
  auto function = std::make_shared<const Fill>(std::move(fill));
  operation.start_result = [function](const ps::ResultProgramQuery&,
                                      const ps::BufferAllocator& allocator) {
    return ps::ResultContinuation::make<Source>(allocator, function);
  };
  return operation;
}
}  // namespace g4_result
