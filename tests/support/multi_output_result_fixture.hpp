#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace multi_result {
struct Failure : std::runtime_error {
  ps::Status status;
  explicit Failure(ps::Status error)
      : std::runtime_error(error.message), status(std::move(error)) {}
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
inline ps::SchemaTemplate schema(
    ps::ElementType type = ps::ElementType::Float64,
    std::vector<std::uint64_t> shape = {1}) {
  ps::SchemaTemplate schema;
  schema.id = "test.multi_output";
  ps::ResultTensorSpec tensor;
  tensor.key = "number";
  tensor.descriptor = {type, std::move(shape)};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
inline ps::WorkflowInputDeclaration declaration(
    unsigned id, std::string name,
    const ps::SchemaTemplate& schema = multi_result::schema()) {
  ps::WorkflowInputDeclaration input;
  input.id = id;
  input.name = std::move(name);
  input.result_schema = std::make_shared<const ps::SchemaTemplate>(schema);
  return input;
}
inline std::size_t width(ps::ElementType type) {
  if (type == ps::ElementType::Float32)
    return 4;
  if (type == ps::ElementType::Float64 || type == ps::ElementType::Int64)
    return 8;
  throw std::runtime_error("unsupported numeric fixture type");
}
inline void write_number(std::uint8_t* bytes, ps::ElementType type,
                         double number) {
  if (type == ps::ElementType::Int64) {
    const auto value = static_cast<std::int64_t>(number);
    std::memcpy(bytes, &value, sizeof(value));
  } else if (type == ps::ElementType::Float32) {
    const auto value = static_cast<float>(number);
    std::memcpy(bytes, &value, sizeof(value));
  } else {
    std::memcpy(bytes, &number, sizeof(number));
  }
}
inline ps::ExecutionBinding binding(
    const ps::ResourceBudget& root, std::string name, double number,
    const ps::SchemaTemplate& schema = multi_result::schema()) {
  auto builder = take(ps::ResultBuilder::start(root, schema, "test.source"));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  const auto count = take(schema.tensors[0].sample_count());
  const auto type = schema.tensors[0].descriptor.element_type;
  const auto size = width(type);
  std::vector<std::uint8_t> bytes(count * size);
  for (std::uint64_t i = 0; i < count; ++i)
    write_number(bytes.data() + i * size, type, number);
  check(builder.publish_tensor(
      0, ps::Region::whole(schema.tensors[0].sample_shape()),
      ps::ByteView(bytes.data(), bytes.size()),
      take(ps::ResultRelation::cartesian(root, count, {})),
      {true, true, true, true}));
  ps::ExecutionBinding binding;
  binding.name = std::move(name);
  binding.result = take(builder.seal());
  return binding;
}
inline double number(const ps::ResultRef& result,
                     std::vector<std::uint64_t> at = {0}) {
  double value = 0;
  check(result.read_tensor(take(result.descriptor()), 0, at, &value,
                           sizeof(value)));
  return value;
}
inline ps::OperationOutputTraits output(
    std::string key,
    const ps::SchemaTemplate& schema = multi_result::schema()) {
  ps::OperationOutputTraits output;
  output.key = std::move(key);
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = std::string(schema.id);
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = schema;
  output.dependency_version = 2;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.maximum_dependency_stages = 2;
  output.continuation_bytes = 256;
  return output;
}
struct Program {
  int input;
  double constant;
  bool check_metadata, requested = false;
  explicit Program(int input, double constant = 0, bool check_metadata = false)
      : input(input), constant(constant), check_metadata(check_metadata) {}
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using namespace ps;  // NOLINT(build/namespaces)
    using Answer = Result<ResultProgramPoll>;
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto shape = spec.sample_shape();
    auto demand =
        phase.query.tensor_outputs.value_or(take(Footprint::all(shape)));
    if (check_metadata &&
        (phase.query.inputs.size() != 2 ||
         phase.query.inputs[0]
                 .result_schema->tensors[0]
                 .descriptor.element_type != ElementType::Float64 ||
         phase.query.inputs[1]
                 .result_schema->tensors[0]
                 .descriptor.element_type != ElementType::Int64))
      return Answer(Status{ErrorCode::TypeMismatch, "original metadata order"});
    if (!requested && input >= 0 && !demand.empty()) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({static_cast<std::uint32_t>(input), 0, demand, 1});
      return Answer(std::move(need));
    }
    if (phase.tensors &&
        phase.tensors->size() !=
            static_cast<std::size_t>(input >= 0 && !demand.empty()))
      return Answer(
          Status{ErrorCode::OperationFailed, "wrong tensor projection"});
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        phase.query.tile_height, phase.query.tile_width,
        phase.query.resources));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    for (const auto& box : demand.boxes()) {
      if (input >= 0) {
        auto window =
            take(phase.tensors->at({static_cast<std::uint32_t>(input), 0})
                     .acquire(box));
        ResultTensorViewTransform identity;
        for (unsigned axis = 0; axis < shape.size(); ++axis)
          identity.source_axes.push_back(
              {static_cast<std::int32_t>(axis), 0, 1, 1});
        auto relation = take(ResultRelation::mapped(
            phase.resources, shape, box, shape, identity.source_axes,
            {static_cast<std::uint32_t>(input), 1, 0, 1,
             ResultSupportTarget::Tensor, 0}));
        check(builder.publish_tensor_view(0, box, window, identity,
                                          std::move(relation),
                                          {true, true, true, true}));
      } else {
        auto count = take(box.element_count());
        check(phase.consume_work(count));
        const auto size = width(spec.descriptor.element_type);
        auto allocation = take(phase.allocator.allocate(count * size));
        for (std::uint64_t i = 0; i < count; ++i)
          write_number(allocation.data() + i * size,
                       spec.descriptor.element_type, constant);
        check(builder.publish_tensor(
            0, box, ps::ByteView(allocation.data(), allocation.size()),
            take(ResultRelation::cartesian(phase.resources,
                                           take(spec.sample_count()), {})),
            {true, true, true, true}));
      }
    }
    return Answer(ResultPublication{take(builder.seal()), true});
  } catch (const Failure& failure) {
    return ps::Result<ps::ResultProgramPoll>(failure.status);
  }
};
}  // namespace multi_result
