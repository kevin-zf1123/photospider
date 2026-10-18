#pragma once

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "photospider/plugin/whole_stage.hpp"

namespace ps::plugin_internal {
inline Result<ResultProgramPoll> independent_result_tile(
    const ResultProgramPhase& phase) {
  const auto stage = phase.query.output_index;
  if (phase.shared_stages) {
    const auto found = phase.shared_stages->find(stage);
    if (found != phase.shared_stages->end()) {
      auto result = sdk::to_tile(phase, found->second);
      if (!result.ok())
        return Result<ResultProgramPoll>(result.status());
      return Result<ResultProgramPoll>(
          ResultPublication{result.take_value(), true});
    }
  }
  return Result<ResultProgramPoll>(
      Status{ErrorCode::InvalidArgument, "compiled shared input is missing"});
}
// Each output has its own full-result stage. This preserves narrower axis
// dependencies and independent arithmetic failures while sharing each values
// computation across all of its requested tiles. The stage ordinal is the
// original output ordinal, independent of the compiled canonical owner step.
inline OperationDefinition independent_results(OperationDefinition definition) {
  const auto traits = definition.traits;
  const auto prepare = definition.prepare_static;
  const auto specialize = definition.specialize_metadata;
  const auto start = definition.start_result;
  const auto mapper = definition.map_dependencies;
  if ((!prepare && !specialize) || !start || !traits.supports_cpu ||
      traits.outputs.empty() || traits.outputs.size() > 64 ||
      traits.supports_gpu || !traits.shared_stages.empty() ||
      !traits.side_effect_free)
    throw std::invalid_argument("invalid independent numeric stages");
  definition.specialize_metadata = {};
  definition.prepare_static =
      [traits, prepare, specialize](
          const auto& inputs,
          const auto& parameters) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    OperationPreparation result;
    if (prepare) {
      auto ready = prepare(inputs, parameters);
      if (!ready.ok())
        return ready;
      result = ready.take_value();
    } else {
      auto ready = specialize(inputs, parameters);
      if (!ready.ok())
        return Answer(ready.status());
      result.outputs = ready.take_value();
    }
    if (result.outputs.size() != traits.outputs.size() ||
        result.shared_stages ||
        result.additional_workspace_bytes > UINT64_MAX - traits.workspace_bytes)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "invalid independent stage preparation"});
    result.shared_stages.emplace();
    for (std::size_t i = 0; i < result.outputs.size(); ++i) {
      const auto& output = result.outputs[i];
      if (!output.metadata.result_schema)
        return Answer(
            Status{ErrorCode::InvalidArgument, "missing stage schema"});
      auto valid = output.metadata.result_schema->validate();
      if (!valid.ok())
        return Answer(valid);
      OperationSharedStage stage;
      stage.key = traits.outputs[i].key;
      stage.result_schema = *output.metadata.result_schema;
      ResultPublicationScope complete{ResultPublicationPolicy::CompleteBundle,
                                      {}};
      for (std::uint32_t member = 0;
           member < stage.result_schema.tensors.size(); ++member)
        complete.members.push_back(member);
      stage.result_schema.publication_scopes = {std::move(complete)};
      stage.validators = traits.outputs[i].output_schema.validators;
      stage.continuation_bytes = traits.outputs[i].continuation_bytes;
      stage.maximum_dependency_stages =
          traits.outputs[i].maximum_dependency_stages;
      stage.workspace_bytes =
          traits.workspace_bytes + result.additional_workspace_bytes;
      stage.maximum_work = UINT64_C(1) << 48;
      const auto& selected = output.input_indices
                                 ? output.input_indices
                                 : traits.outputs[i].input_indices;
      if (selected) {
        stage.input_indices = *selected;
      } else {
        for (std::uint32_t port = 0; port < inputs.size(); ++port)
          stage.input_indices.push_back(port);
      }
      std::sort(stage.input_indices.begin(), stage.input_indices.end());
      if (traits.outputs[i].maximum_output_payload_bytes) {
        stage.maximum_output_payload_bytes =
            *traits.outputs[i].maximum_output_payload_bytes;
      } else {
        for (const auto& tensor : stage.result_schema.tensors) {
          auto count = tensor.sample_count();
          const auto width =
              TensorSpan::element_size(tensor.descriptor.element_type);
          if (!count.ok() ||
              count.value() >
                  (UINT64_MAX - stage.maximum_output_payload_bytes) / width)
            return Answer(
                Status{ErrorCode::ResourceExhausted, "stage payload overflow"});
          stage.maximum_output_payload_bytes += count.value() * width;
        }
      }
      result.shared_stages->push_back(std::move(stage));
    }
    result.additional_workspace_bytes = 0;
    return Answer(std::move(result));
  };
  definition.traits.workspace_bytes = 0;
  for (auto& output : definition.traits.outputs) {
    output.continuation_bytes = 1;
    output.maximum_dependency_stages = 2;
    output.output_schema.validators.clear();
  }
  definition.map_dependencies =
      [mapper](const ResultStaticQuery& query) -> Result<ResultStaticMapping> {
    if (query.shared_stage != UINT32_MAX) {
      auto routed = query;
      routed.output_index = query.shared_stage;
      return mapper(routed);
    }
    ResultStaticMapping result;
    result.shared_stages.push_back(ResultSharedNeed{query.output_index});
    return Result<ResultStaticMapping>(std::move(result));
  };
  definition.start_result = [start](const ResultProgramQuery& query,
                                    const BufferAllocator& allocator) {
    if (query.shared_stage == UINT32_MAX)
      return ResultContinuation::stateless<&independent_result_tile>();
    auto routed = query;
    routed.output_index = query.shared_stage;
    return start(routed, allocator);
  };
  return definition;
}
}  // namespace ps::plugin_internal
