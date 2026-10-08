#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "plugin/port_validation.hpp"
#include "plugin/result_c_codec.hpp"
#include "plugin/result_c_joint.hpp"
#include "plugin/result_c_member.hpp"
#include "plugin/result_plugin.hpp"

namespace ps::plugin_internal {
using result_c::array;
using result_c::c_joint_storage_bytes;
using result_c::c_member_storage_bytes;
using result_c::constraint;
using result_c::Definition;
using result_c::invalid;
using result_c::port;
using result_c::prepare_metadata;
using result_c::start_c_joint;
using result_c::start_c_member;
using result_c::text;
Result<std::vector<OperationDefinition>> import_result_plugin(
    const ps_result_operation_plugin_api_v2* api,
    std::shared_ptr<void> library) {
  if (!api ||
      reinterpret_cast<std::uintptr_t>(api) %
          alignof(ps_result_operation_plugin_api_v2) ||
      api->struct_size != sizeof(*api) ||
      api->abi_version != PS_RESULT_OPERATION_ABI_VERSION_2 ||
      !array(api->operations, api->operation_count, 1024) ||
      !api->operation_count || !api->destroy)
    return Result<std::vector<OperationDefinition>>(
        invalid("invalid Result ABI table"));
  std::vector<OperationDefinition> definitions;
  for (std::uint32_t i = 0; i < api->operation_count; ++i) {
    const auto& op = api->operations[i];
    if (op.struct_size != sizeof(op) ||
        !array(op.inputs, op.input_count, 1024) || op.repeated_match > 1 ||
        (op.repeated_maximum ? (!op.repeated_minimum ||
                                (!op.repeated_match && !op.resolve_metadata) ||
                                op.repeated_minimum > op.repeated_maximum ||
                                op.repeated_maximum > 1024 ||
                                op.input_count > 1024 - op.repeated_maximum ||
                                !array(op.repeated_input, 1, 1))
                             : (op.repeated_input || op.repeated_minimum ||
                                op.repeated_match)) ||
        !array(op.outputs, op.output_count, 64) || !op.output_count ||
        !op.start || !op.poll || !op.destroy || op.state_bytes > 1048576 ||
        !op.maximum_stages || op.maximum_stages > 1048576 ||
        op.cpu_staged_tiles > 1 || op.data_movement > 1 ||
        (op.flags &
         ~(PS_RESULT_FLAG_DETERMINISTIC_V2 |
           PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2 | PS_RESULT_FLAG_CPU_V2 |
           PS_RESULT_FLAG_GPU_V2 | PS_RESULT_FLAG_CPU_FALLBACK_V2 |
           PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V2)))
      return Result<std::vector<OperationDefinition>>(
          invalid("invalid Result operation table"));
    if (op.joint &&
        (!array(op.joint, 1, 1) || op.joint->struct_size != sizeof(*op.joint) ||
         (op.joint->contract != 1 && op.joint->contract != 2) ||
         op.joint->query_size != sizeof(ps_result_joint_query_v2) ||
         op.joint->member_size != sizeof(ps_result_joint_member_v2) ||
         op.joint->outcome_size != sizeof(ps_result_joint_outcome_v2) ||
         op.joint->services_size != sizeof(ps_result_joint_services_v2) ||
         !op.joint->state_bytes || op.joint->state_bytes > 1048576 ||
         !op.joint->start || !op.joint->poll || !op.joint->destroy ||
         (op.joint->contract == 1 && op.output_count < 2) ||
         !(op.flags & PS_RESULT_FLAG_CPU_V2)))
      return Result<std::vector<OperationDefinition>>(
          invalid("invalid C Result joint table"));
    auto key = text(op.key, op.key_size);
    if (!key.ok())
      return Result<std::vector<OperationDefinition>>(key.status());
    OperationDefinition definition;
    definition.key = key.take_value();
    auto& traits = definition.traits;
    traits.input_count = op.input_count;
    traits.repeated_minimum = op.repeated_minimum;
    traits.repeated_maximum = op.repeated_maximum;
    traits.repeated_match = !op.repeated_maximum || op.repeated_match != 0;
    traits.supports_cpu = (op.flags & PS_RESULT_FLAG_CPU_V2) != 0;
    traits.supports_gpu = (op.flags & PS_RESULT_FLAG_GPU_V2) != 0;
    traits.allows_cpu_fallback =
        (op.flags & PS_RESULT_FLAG_CPU_FALLBACK_V2) != 0;
    traits.deterministic = (op.flags & PS_RESULT_FLAG_DETERMINISTIC_V2) != 0;
    traits.side_effect_free =
        (op.flags & PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2) != 0;
    traits.cacheable = traits.deterministic && traits.side_effect_free;
    traits.share_blocks_across_outputs =
        (op.flags & PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V2) != 0;
    traits.cpu_staged_tiles = op.cpu_staged_tiles != 0;
    traits.workspace_bytes = op.workspace_bytes;
    traits.input_schema.clear();
    traits.outputs.clear();
    std::vector<OperationMetadata> input_prototypes, output_prototypes;
    const auto prototypes = op.input_count + (op.repeated_maximum ? 1U : 0U);
    for (std::uint32_t input = 0; input < prototypes; ++input) {
      const auto& declared =
          input < op.input_count ? op.inputs[input] : *op.repeated_input;
      auto copied = constraint(declared);
      if (!copied.ok())
        return Result<std::vector<OperationDefinition>>(copied.status());
      traits.input_schema.push_back(copied.take_value());
      if (declared.kind == PS_RESULT_OBJECT_V2 && !declared.schema) {
        input_prototypes.emplace_back();
      } else {
        auto metadata = port(declared);
        if (!metadata.ok())
          return Result<std::vector<OperationDefinition>>(metadata.status());
        input_prototypes.push_back(metadata.take_value());
      }
    }
    for (std::uint32_t output = 0; output < op.output_count; ++output) {
      const auto& declared = op.outputs[output];
      if (declared.struct_size != sizeof(declared) ||
          (declared.flags & ~(PS_RESULT_OUTPUT_PRESERVE_VIEWS_V2 |
                              PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V2 |
                              PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2)) ||
          (!(declared.flags & PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2) &&
           declared.maximum_output_payload_bytes) ||
          (declared.execution != 1 && declared.execution != 2) ||
          declared.observation_kind > PS_RESULT_REQUEST_RECORD_V2 ||
          declared.failure_delivery !=
              (op.joint && op.joint->contract == 2
                   ? PS_RESULT_PER_ATOM_OUTCOME_V2
                   : PS_RESULT_REQUEST_FAILURE_ONLY_V2) ||
          (declared.input_count != UINT32_MAX &&
           !array(declared.input_indices, declared.input_count, 1024)) ||
          (declared.input_count == UINT32_MAX && declared.input_indices))
        return Result<std::vector<OperationDefinition>>(
            invalid("invalid named Result output"));
      auto name = text(declared.key, declared.key_size);
      if (!name.ok())
        return Result<std::vector<OperationDefinition>>(name.status());
      auto metadata = port(declared.port);
      if (!metadata.ok())
        return Result<std::vector<OperationDefinition>>(metadata.status());
      auto output_constraint = constraint(declared.port);
      if (!output_constraint.ok())
        return Result<std::vector<OperationDefinition>>(
            output_constraint.status());
      OperationOutputTraits contract;
      contract.output_schema = output_constraint.take_value();
      contract.key = name.take_value();
      contract.observation_kind =
          static_cast<ObservationKind>(declared.observation_kind);
      contract.failure_delivery =
          static_cast<FailureDelivery>(declared.failure_delivery);
      contract.preserve_output_views =
          (declared.flags & PS_RESULT_OUTPUT_PRESERVE_VIEWS_V2) != 0;
      contract.requires_input_views =
          (declared.flags & PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V2) != 0;
      if (declared.flags & PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2)
        contract.maximum_output_payload_bytes =
            declared.maximum_output_payload_bytes;
      contract.data_movement = op.data_movement
                                   ? DataMovementKind::BitwiseMapped
                                   : DataMovementKind::None;
      contract.region_rule = declared.execution == 1
                                 ? OperationRegionRule::Whole
                                 : OperationRegionRule::Dependency;
      contract.continuation_bytes =
          c_member_storage_bytes() + std::max<std::uint64_t>(1, op.state_bytes);
      contract.maximum_dependency_stages = op.maximum_stages;
      if (declared.input_count != UINT32_MAX) {
        contract.input_indices = std::vector<std::uint32_t>{};
        if (declared.input_count)
          contract.input_indices->assign(
              declared.input_indices,
              declared.input_indices + declared.input_count);
      }
      contract.result_schema = *metadata.value().result_schema;
      contract.output_schema.kind = OperationPortKind::Result;
      contract.output_schema.result_schema_id =
          std::string(metadata.value().result_schema->id);
      contract.output_schema.result_schema_version =
          metadata.value().result_schema->version;
      output_prototypes.push_back(metadata.take_value());
      traits.outputs.push_back(std::move(contract));
    }
    if (!array(op.parameters, op.parameter_count, 128))
      return Result<std::vector<OperationDefinition>>(
          invalid("invalid Result parameter table"));
    for (std::uint32_t p = 0; p < op.parameter_count; ++p) {
      const auto& parameter = op.parameters[p];
      if (parameter.struct_size != sizeof(parameter) ||
          parameter.required > 1 || parameter.bounded > 1)
        return Result<std::vector<OperationDefinition>>(
            invalid("invalid Result parameter"));
      auto name = text(parameter.key, parameter.key_size);
      if (!name.ok())
        return Result<std::vector<OperationDefinition>>(name.status());
      traits.parameter_schema.push_back(
          {name.take_value(),
           static_cast<OperationParameterType>(parameter.type),
           parameter.required != 0, parameter.bounded != 0, parameter.minimum,
           parameter.maximum});
    }
    // Callbacks retain this trait copy before registry publication; they need
    // the same canonical parameter order as the registered definition.
    std::sort(traits.parameter_schema.begin(), traits.parameter_schema.end(),
              [](const OperationParameterSpec& left,
                 const OperationParameterSpec& right) {
                return left.key < right.key;
              });
    traits.requires_metadata_specialization = true;
    if (op.joint) {
      traits.joint_contract = op.joint->contract;
      traits.joint_continuation_bytes =
          c_joint_storage_bytes() + op.joint->state_bytes;
      traits.joint_workspace_bytes = op.joint->workspace_bytes;
    }
    auto owner =
        std::make_shared<Definition>(Definition{op,
                                                {},
                                                library,
                                                traits,
                                                std::move(input_prototypes),
                                                std::move(output_prototypes)});
    if (op.resolve_metadata) {
      definition.prepare_static =
          [owner](const std::vector<OperationMetadata>& inputs,
                  const std::map<std::string, ParameterValue>& parameters) {
            return prepare_metadata(*owner, inputs, parameters);
          };
    }
    const auto validate_inputs =
        [owner](const std::vector<OperationMetadata>& inputs,
                const std::map<std::string, ParameterValue>& parameters) {
          auto resolved = resolve_operation_traits(owner->traits, inputs.size(),
                                                   parameters);
          if (!resolved.ok())
            return resolved.status();
          if (owner->api.resolve_metadata)
            return Status::success();
          for (std::uint32_t i = 0; i < inputs.size(); ++i) {
            auto valid = input_internal::validate_port_metadata(
                resolved.value().input_schema[i], inputs[i]);
            if (!valid.ok())
              return valid;
            const auto prototype =
                owner->traits.repeated_maximum && i >= owner->api.input_count
                    ? owner->api.input_count
                    : i;
            const auto& expected = owner->input_prototypes.at(prototype);
            if (expected.result_schema) {
              if (!inputs[i].result_schema ||
                  !inputs[i].result_schema->same_schema(
                      *expected.result_schema))
                return Status{ErrorCode::TypeMismatch,
                              "C Result input schema mismatch"};
            }
          }
          return Status::success();
        };
    if (!op.resolve_metadata) {
      definition.specialize_metadata =
          [owner, validate_inputs](const auto& inputs, const auto& parameters)
          -> Result<std::vector<OperationOutputSpecialization>> {
        auto status = validate_inputs(inputs, parameters);
        if (!status.ok())
          return Result<std::vector<OperationOutputSpecialization>>(status);
        std::vector<OperationOutputSpecialization> outputs;
        for (const auto& prototype : owner->output_prototypes) {
          OperationOutputSpecialization output;
          output.metadata = prototype;
          outputs.push_back(std::move(output));
        }
        return Result<std::vector<OperationOutputSpecialization>>(
            std::move(outputs));
      };
    }
    owner->api.joint = nullptr;
    if (op.joint)
      owner->joint = *op.joint;
    definition.start_result = [owner](const ResultProgramQuery&,
                                      const BufferAllocator& allocator) {
      return start_c_member(owner, allocator);
    };
    if (op.joint) {
      definition.start_result_joint =
          [owner](const ResourceVector<ResultProgramQuery>& queries,
                  const BufferAllocator& allocator) {
            return start_c_joint(owner, queries, allocator);
          };
    }
    definitions.push_back(std::move(definition));
  }
  return Result<std::vector<OperationDefinition>>(std::move(definitions));
}
}  // namespace ps::plugin_internal
