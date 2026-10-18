#pragma once

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/plugin/whole_stage.hpp"

namespace ps::plugin_internal {
inline Result<ResultProgramPoll> joint_result_tile(
    const ResultProgramPhase& phase) {
  if (phase.shared_stages) {
    const auto found = phase.shared_stages->find(0);
    if (found != phase.shared_stages->end()) {
      auto result =
          sdk::to_tile(phase, found->second, phase.query.output_index);
      if (!result.ok())
        return Result<ResultProgramPoll>(result.status());
      return Result<ResultProgramPoll>(
          ResultPublication{result.take_value(), true});
    }
  }
  return Result<ResultProgramPoll>(
      Status{ErrorCode::InvalidArgument, "compiled shared input is missing"});
}
// Joint numeric algorithms explicitly compute one member for each public
// output. All outputs must have the same potential input projection. Algorithms
// with a descriptor-only axis output and additional values inputs use separate
// stages.
inline OperationDefinition joint_result(OperationDefinition definition) {
  const auto traits = definition.traits;
  const auto specialize = definition.specialize_metadata;
  const auto start = definition.start_result;
  const auto mapper = definition.map_dependencies;
  if (traits.outputs.empty() || traits.outputs.size() > 16 || !specialize ||
      definition.prepare_static || !start || !traits.supports_cpu ||
      traits.supports_gpu || !traits.shared_stages.empty() ||
      !traits.side_effect_free)
    throw std::invalid_argument("invalid joint numeric stage");
  for (const auto& output : traits.outputs)
    if (!output.result_schema || output.result_schema->tensors.size() != 1 ||
        !output.output_schema.validators.empty())
      throw std::invalid_argument(
          "joint numeric stage requires unvalidated single-member outputs");
  definition.specialize_metadata = {};
  definition.prepare_static =
      [traits, specialize](
          const auto& inputs,
          const auto& parameters) -> Result<OperationPreparation> {
    auto resolved = specialize(inputs, parameters);
    if (!resolved.ok())
      return Result<OperationPreparation>(resolved.status());
    OperationPreparation prepared;
    prepared.outputs = resolved.take_value();
    if (prepared.outputs.size() != traits.outputs.size())
      return Result<OperationPreparation>(
          Status{ErrorCode::InvalidArgument, "joint output count mismatch"});
    OperationSharedStage stage;
    stage.key = "joint";
    stage.result_schema.id = "photospider.numeric.joint";
    stage.continuation_bytes = traits.outputs[0].continuation_bytes;
    stage.maximum_dependency_stages =
        traits.outputs[0].maximum_dependency_stages;
    stage.workspace_bytes = traits.workspace_bytes;
    stage.maximum_work = UINT64_C(1) << 48;
    for (std::size_t index = 0; index < prepared.outputs.size(); ++index) {
      const auto& output = prepared.outputs[index];
      if (!output.metadata.result_schema ||
          output.metadata.result_schema->tensors.size() != 1)
        return Result<OperationPreparation>(
            Status{ErrorCode::InvalidArgument, "invalid joint member"});
      auto valid = output.metadata.result_schema->validate();
      if (!valid.ok())
        return Result<OperationPreparation>(valid);
      const auto& selected = output.input_indices
                                 ? output.input_indices
                                 : traits.outputs[index].input_indices;
      std::vector<std::uint32_t> projection;
      if (selected) {
        projection = *selected;
      } else {
        for (std::uint32_t port = 0; port < inputs.size(); ++port)
          projection.push_back(port);
      }
      std::sort(projection.begin(), projection.end());
      if (index && projection != stage.input_indices)
        return Result<OperationPreparation>(Status{
            ErrorCode::InvalidArgument, "joint output projections differ"});
      stage.input_indices = std::move(projection);
      auto tensor = output.metadata.result_schema->tensors[0];
      tensor.key = traits.outputs[index].key;
      auto count = tensor.sample_count();
      const auto width =
          TensorSpan::element_size(tensor.descriptor.element_type);
      if (!count.ok() ||
          count.value() >
              (UINT64_MAX - stage.maximum_output_payload_bytes) / width)
        return Result<OperationPreparation>(
            Status{ErrorCode::ResourceExhausted, "joint payload overflow"});
      stage.maximum_output_payload_bytes += count.value() * width;
      stage.result_schema.tensors.push_back(std::move(tensor));
    }
    ResultPublicationScope complete{ResultPublicationPolicy::CompleteBundle,
                                    {}};
    for (std::uint32_t member = 0; member < stage.result_schema.tensors.size();
         ++member)
      complete.members.push_back(member);
    stage.result_schema.publication_scopes = {std::move(complete)};
    prepared.shared_stages =
        std::vector<OperationSharedStage>{std::move(stage)};
    return Result<OperationPreparation>(std::move(prepared));
  };
  definition.traits.workspace_bytes = 0;
  for (auto& output : definition.traits.outputs) {
    output.continuation_bytes = 1;
    output.maximum_dependency_stages = 2;
  }
  definition.map_dependencies =
      [mapper](const ResultStaticQuery& query) -> Result<ResultStaticMapping> {
    if (query.shared_stage != UINT32_MAX) {
      return mapper(query);
    }
    ResultStaticMapping result;
    result.shared_stages.push_back(ResultSharedNeed{0});
    return Result<ResultStaticMapping>(std::move(result));
  };
  definition.start_result = [start](const ResultProgramQuery& query,
                                    const BufferAllocator& allocator) {
    if (query.shared_stage != UINT32_MAX)
      return start(query, allocator);
    return ResultContinuation::stateless<&joint_result_tile>();
  };
  return definition;
}
}  // namespace ps::plugin_internal
