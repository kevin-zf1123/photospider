#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace atom_result {
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
inline ps::SchemaTemplate schema(unsigned width = 3) {
  ps::SchemaTemplate result;
  result.id = "example.atom_numbers";
  ps::ResultTensorSpec tensor;
  tensor.key = "number";
  tensor.descriptor = {ps::ElementType::Float64, {width}};
  result.tensors.push_back(std::move(tensor));
  return result;
}
inline ps::WorkflowInputDeclaration declaration(unsigned id, std::string name) {
  ps::WorkflowInputDeclaration input;
  input.id = id;
  input.name = std::move(name);
  input.result_schema = std::make_shared<const ps::SchemaTemplate>(schema());
  return input;
}
inline ps::ExecutionBinding binding(const ps::ResourceBudget& root,
                                    std::string name,
                                    const std::array<double, 3>& values) {
  auto builder = take(ps::ResultBuilder::start(root, schema(), "source"));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  check(builder.publish_tensor(
      0, ps::Region::whole({3}),
      {reinterpret_cast<const std::uint8_t*>(values.data()), sizeof(values)},
      take(ps::ResultRelation::cartesian(root, 3, {})),
      {true, true, true, true}));
  ps::ExecutionBinding input;
  input.name = std::move(name);
  input.result = take(builder.seal());
  return input;
}
inline ps::ResultProgramPoll publish(const ps::ResultProgramPhase& phase,
                                     double value, unsigned port = 0,
                                     bool supported = true,
                                     bool guarded = false) {
  const auto key = take(ps::result_atom_key(phase.query));
  auto builder = take(ps::ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{}));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
  const auto width =
      take(phase.query.output.result_schema->tensors[0].sample_count());
  auto relation = take(ps::ResultRelation::cartesian(
      phase.resources, width,
      supported ? ps::ResultSupport{port, guarded && port == 0 ? 3u : 1u,
                                    key.coordinate[0], 1,
                                    ps::ResultSupportTarget::Tensor, 0}
                : ps::ResultSupport{}));
  if (supported && port == 1) {
    auto control = take(ps::ResultRelation::cartesian(
        phase.resources, width,
        {0, 2, key.coordinate[0], 1, ps::ResultSupportTarget::Tensor, 0}));
    relation =
        take(ps::ResultRelation::unite(phase.resources, {relation, control}));
  }
  check(builder.publish_tensor(
      0, phase.query.tensor_outputs->boxes()[0],
      {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
      std::move(relation), {true, true, true, true}));
  return ps::ResultPublication{take(builder.seal()), true};
}
struct Coordinates {
  int mode;
  std::array<unsigned, 3> stage{};
  explicit Coordinates(int mode) : mode(mode) {}
  ps::Result<ps::ResultProgramPoll> member(
      const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    const auto key = take(ps::result_atom_key(phase.query));
    const auto index = key.coordinate[0];
    auto& step = stage.at(index);
    const auto need = [&](unsigned port) {
      ps::ResultProgramNeed request;
      request.tensors.push_back(
          {port, 0,
           take(ps::Footprint::from_regions({3}, {ps::Region({{index, 1}})})),
           port == 0 && mode >= 0 ? 3u : 1u});
      return Answer(std::move(request));
    };
    if (!step) {
      step = 1;
      return need(0);
    }
    const unsigned port = step == 1 ? 0 : 1;
    double value = 0;
    auto read = phase.tensors->at({port, 0}).read({index}, &value, 8);
    if (!read.ok())
      return Answer(read);
    if (mode == -2) {
      if (index == 1)
        return Answer(
            ps::Status{ps::ErrorCode::OperationFailed,
                       "source transport failed",
                       ps::FailureReason::ShortIo,
                       {ps::FailureOrigin::Io, ps::FailureScope::Group}});
      return Answer(publish(phase, value));
    }
    if (mode == -1)
      return Answer(publish(phase, value * 2));
    if (step == 1 && value < 0) {
      step = 2;
      return need(1);
    }
    if (value == 0)
      return Answer(
          ps::Status{ps::ErrorCode::OperationFailed,
                     "zero denominator",
                     ps::FailureReason::DivideByZero,
                     {ps::FailureOrigin::Domain, ps::FailureScope::Atom, key}});
    return Answer(publish(phase, 1 / value, port, true, true));
  }
  ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
      const ps::ResultJointPhase& phase) {
    ps::ResourceVector<ps::ResultJointOutcome> outcomes;
    for (const auto* member_phase : phase.members)
      outcomes.push_back({take(ps::result_atom_key(member_phase->query)),
                          member(*member_phase)});
    if (mode == 1 && phase.members.front()->tensors &&
        !phase.members.front()->tensors->empty())
      outcomes.pop_back();
    return ps::Result<ps::ResourceVector<ps::ResultJointOutcome>>(
        std::move(outcomes));
  }
};
struct Single {
  Coordinates state;
  explicit Single(int mode) : state(mode) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    return state.member(phase);
  }
};
inline ps::OperationDefinition operation(int mode, std::string key) {
  ps::OperationDefinition operation;
  operation.key = std::move(key);
  operation.traits.input_count = mode < 0 ? 1 : 2;
  operation.traits.input_schema.resize(operation.traits.input_count);
  for (auto& input : operation.traits.input_schema) {
    input.kind = ps::OperationPortKind::Result;
    input.result_schema_id = "example.atom_numbers";
    input.result_schema_version = 1;
  }
  auto& output = operation.traits.outputs[0];
  output.output_schema = operation.traits.input_schema[0];
  output.result_schema = schema();
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(Single);
  output.maximum_dependency_stages = 8;
  output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  operation.traits.workspace_bytes = 16384;
  operation.traits.joint_contract = 2;
  operation.traits.joint_continuation_bytes = sizeof(Coordinates);
  operation.traits.joint_workspace_bytes = 16384;
  operation.start_result = [mode](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<Single>(allocator, mode);
  };
  operation.start_result_joint = [mode](const auto&, const auto& allocator) {
    return ps::ResultJointContinuation::make<Coordinates>(allocator, mode);
  };
  if (mode == -2) {
    operation.traits.joint_contract = 0;
    operation.traits.joint_continuation_bytes = 0;
    operation.traits.joint_workspace_bytes = 0;
    operation.traits.outputs[0].failure_delivery =
        ps::FailureDelivery::RequestFailureOnly;
    operation.start_result_joint = {};
  }
  return operation;
}
}  // namespace atom_result
