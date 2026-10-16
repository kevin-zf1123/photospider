#pragma once

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/tensor_helpers.hpp"

namespace ps::sdk {
/** @brief A tile continuation borrowing one complete Run-shared Result.
 * The Need grants only the stage Result. Original input read authority is not
 * inherited. The host attaches the stage's frozen dependency evidence.
 */
inline Result<ResultProgramPoll> shared_result_tile(
    const ResultProgramPhase& phase) {
  if (phase.shared_stages) {
    const auto found = phase.shared_stages->find(0);
    if (found != phase.shared_stages->end()) {
      auto tile = to_tile(phase, found->second);
      if (!tile.ok())
        return Result<ResultProgramPoll>(tile.status());
      return Result<ResultProgramPoll>(
          ResultPublication{tile.take_value(), true});
    }
  }
  ResultProgramNeed need;
  need.shared_stages = ResourceVector<ResultSharedNeed>(
      ResourceAllocator<ResultSharedNeed>(phase.resources));
  need.shared_stages.push_back(0);
  return Result<ResultProgramPoll>(std::move(need));
}
/** @brief Adapt a single-output full-tensor algorithm to a Run-shared stage.
 * This is an explicit SDK policy, selected by the operation author. The
 * original factory runs once per Run on CPU, with complete output schema and no
 * selected tile. It remains responsible for nonblocking input Needs, numerical
 * policy, and resource-bounded computation. Output callbacks only publish
 * retained tile views. Preparation fixes the projection, schema and all stage
 * bounds. No dense conversion, cache, spill, or implicit recomputation is
 * introduced.
 *
 * The definition must have one CPU-only output and no existing shared stages.
 * work_bound is the cumulative stage work allowance, including publication
 * accounting. Existing preparation and its immutable state are retained.
 * Throws invalid_argument for an unsupported definition.
 * Registration/preparation report invalid schema, bound overflow and allocation
 * failures normally.
 */
inline OperationDefinition with_whole_stage(OperationDefinition definition,
                                            std::uint64_t work_bound) {
  if (definition.traits.outputs.size() != 1 ||
      !definition.traits.supports_cpu || definition.traits.supports_gpu ||
      !definition.traits.shared_stages.empty() || !work_bound ||
      !definition.traits.outputs[0].result_schema || !definition.start_result)
    throw std::invalid_argument(
        "whole stage requires one CPU output and a finite work bound");
  const auto traits = definition.traits;
  const auto prepare = definition.prepare_static;
  const auto specialize = definition.specialize_metadata;
  const auto start = definition.start_result;
  definition.traits.requires_metadata_specialization = true;
  definition.specialize_metadata = {};
  definition.prepare_static =
      [traits, prepare, specialize, work_bound](
          const auto& inputs,
          const auto& parameters) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    OperationPreparation result;
    if (prepare) {
      auto ready = prepare(inputs, parameters);
      if (!ready.ok())
        return ready;
      result = ready.take_value();
    } else if (specialize) {
      auto ready = specialize(inputs, parameters);
      if (!ready.ok())
        return Answer(ready.status());
      result.outputs = ready.take_value();
    } else {
      OperationOutputSpecialization output;
      output.metadata.result_schema = std::make_shared<const SchemaTemplate>(
          *traits.outputs[0].result_schema);
      result.outputs.push_back(std::move(output));
    }
    if (result.outputs.size() != 1 ||
        !result.outputs[0].metadata.result_schema ||
        (result.shared_stages && !result.shared_stages->empty()))
      return Answer(Status{ErrorCode::InvalidArgument,
                           "invalid whole-stage preparation"});
    auto schema_status =
        result.outputs[0].metadata.result_schema->validate(true);
    if (!schema_status.ok())
      return Answer(schema_status);
    const auto overflow = [] {
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "whole-stage static bound overflows",
                           FailureReason::CapacityLimit});
    };
    OperationSharedStage stage;
    stage.key = "whole";
    stage.validators = traits.outputs[0].output_schema.validators;
    stage.result_schema = *result.outputs[0].metadata.result_schema;
    stage.continuation_bytes = traits.outputs[0].continuation_bytes;
    stage.maximum_dependency_stages =
        traits.outputs[0].maximum_dependency_stages;
    stage.maximum_work = work_bound;
    stage.mandatory = !traits.side_effect_free;
    const auto& projection = result.outputs[0].input_indices
                                 ? result.outputs[0].input_indices
                                 : traits.outputs[0].input_indices;
    if (projection) {
      stage.input_indices = *projection;
    } else {
      for (std::uint32_t port = 0; port < inputs.size(); ++port)
        stage.input_indices.push_back(port);
    }
    std::sort(stage.input_indices.begin(), stage.input_indices.end());
    if (result.additional_workspace_bytes > UINT64_MAX - traits.workspace_bytes)
      return overflow();
    stage.workspace_bytes =
        traits.workspace_bytes + result.additional_workspace_bytes;
    for (auto port : stage.input_indices) {
      if (port >= inputs.size() || !inputs[port].result_schema)
        return Answer(Status{ErrorCode::InvalidArgument,
                             "invalid whole-stage input projection"});
      if (!traits.workspace_input_multiplier)
        continue;
      for (const auto& tensor : inputs[port].result_schema->tensors) {
        auto count = tensor.sample_count();
        const auto width =
            TensorSpan::element_size(tensor.descriptor.element_type) *
            traits.workspace_input_multiplier;
        if (!count.ok() ||
            count.value() > (UINT64_MAX - stage.workspace_bytes) / width)
          return overflow();
        stage.workspace_bytes += count.value() * width;
      }
    }
    if (traits.outputs[0].maximum_output_payload_bytes) {
      stage.maximum_output_payload_bytes =
          *traits.outputs[0].maximum_output_payload_bytes;
    } else {
      for (const auto& tensor : stage.result_schema.tensors) {
        auto count = tensor.sample_count();
        const auto width =
            TensorSpan::element_size(tensor.descriptor.element_type);
        if (!count.ok() ||
            count.value() >
                (UINT64_MAX - stage.maximum_output_payload_bytes) / width)
          return overflow();
        stage.maximum_output_payload_bytes += count.value() * width;
      }
    }
    result.shared_stages = std::vector<OperationSharedStage>{std::move(stage)};
    // The original scratch belongs to the stage; tile views need no scratch.
    result.additional_workspace_bytes = 0;
    return Answer(std::move(result));
  };
  // Static prototypes also retain mandatory roots before specialization.
  OperationSharedStage prototype;
  prototype.key = "whole";
  prototype.validators = traits.outputs[0].output_schema.validators;
  prototype.result_schema = *traits.outputs[0].result_schema;
  prototype.continuation_bytes = traits.outputs[0].continuation_bytes;
  prototype.maximum_dependency_stages =
      traits.outputs[0].maximum_dependency_stages;
  prototype.maximum_work = work_bound;
  prototype.mandatory = !traits.side_effect_free;
  if (traits.outputs[0].input_indices)
    prototype.input_indices = *traits.outputs[0].input_indices;
  std::sort(prototype.input_indices.begin(), prototype.input_indices.end());
  definition.traits.shared_stages = {std::move(prototype)};
  definition.traits.workspace_bytes = 0;
  definition.traits.workspace_input_multiplier = 0;
  definition.traits.outputs[0].output_schema.validators.clear();
  definition.traits.outputs[0].continuation_bytes = 1;
  definition.traits.outputs[0].maximum_dependency_stages = 2;
  definition.start_result = [start](const ResultProgramQuery& query,
                                    const BufferAllocator& allocator) {
    if (query.shared_stage != UINT32_MAX)
      return start(query, allocator);
    return ResultContinuation::stateless<&shared_result_tile>();
  };
  return definition;
}
}  // namespace ps::sdk
