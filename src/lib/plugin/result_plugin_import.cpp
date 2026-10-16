#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/checked_math.hpp"
#include "plugin/port_validation.hpp"
#include "plugin/result_c_codec.hpp"
#include "plugin/result_c_member.hpp"
#include "plugin/result_plugin.hpp"

namespace ps::plugin_internal {
using result_c::array;
using result_c::c_member_storage_bytes;
using result_c::constraint;
using result_c::Definition;
using result_c::invalid;
using result_c::port;
using result_c::prepare_metadata;
using result_c::start_c_member;
using result_c::text;
Result<ImportedResultPlugin> import_result_plugin(
    const ps_result_operation_plugin_api_v5* api,
    std::shared_ptr<void> library) {
  if (!api ||
      reinterpret_cast<std::uintptr_t>(api) %
          alignof(ps_result_operation_plugin_api_v5) ||
      api->struct_size != sizeof(*api) ||
      api->abi_version != PS_RESULT_OPERATION_ABI_VERSION_5 ||
      !array(api->operations, api->operation_count, 1024) ||
      !api->operation_count || !api->destroy)
    return Result<ImportedResultPlugin>(invalid("invalid Result ABI table"));
  ImportedResultPlugin definitions;
  if (!array(api->validators, api->validator_count, 64) ||
      !array(api->diagnostics, api->diagnostic_count, 64))
    return Result<ImportedResultPlugin>(invalid("invalid extension tables"));
  for (uint32_t i = 0; i < api->validator_count; ++i) {
    auto value = result_c::validation_definition(api->validators[i], library);
    if (!value.ok())
      return Result<ImportedResultPlugin>(value.status());
    definitions.validators.push_back(value.take_value());
  }
  for (uint32_t i = 0; i < api->diagnostic_count; ++i) {
    auto value = result_c::diagnostic_definition(api->diagnostics[i], library);
    if (!value.ok())
      return Result<ImportedResultPlugin>(value.status());
    definitions.diagnostics.push_back(value.take_value());
  }
  for (std::uint32_t i = 0; i < api->operation_count; ++i) {
    const auto& op = api->operations[i];
    if (op.struct_size != sizeof(op) ||
        !array(op.inputs, op.input_count, 1024) || op.repeated_match > 1 ||
        (op.repeated_maximum
             ? (!op.repeated_minimum ||
                (!op.repeated_match && !op.resolve_metadata) ||
                op.repeated_minimum > op.repeated_maximum ||
                op.repeated_maximum > 1024 ||
                !core_internal::can_add(op.input_count, op.repeated_maximum,
                                        1024) ||
                !array(op.repeated_input, 1, 1))
             : (op.repeated_input || op.repeated_minimum ||
                op.repeated_match)) ||
        !array(op.outputs, op.output_count, 64) || !op.output_count ||
        !op.start || !op.poll || !op.destroy || op.state_bytes > 1048576 ||
        !op.maximum_stages || op.maximum_stages > 1048576 ||
        op.cpu_staged_tiles > 1 ||
        (op.flags &
         ~(PS_RESULT_FLAG_DETERMINISTIC_V5 |
           PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5 | PS_RESULT_FLAG_CPU_V5 |
           PS_RESULT_FLAG_GPU_V5 | PS_RESULT_FLAG_CPU_FALLBACK_V5 |
           PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V5)))
      return Result<ImportedResultPlugin>(
          invalid("invalid Result operation table"));
    auto key = text(op.key, op.key_size);
    if (!key.ok())
      return Result<ImportedResultPlugin>(key.status());
    OperationDefinition definition;
    definition.key = key.take_value();
    auto& traits = definition.traits;
    auto diagnostics =
        result_c::extension_ids(op.diagnostics, op.diagnostic_count);
    if (!diagnostics.ok())
      return Result<ImportedResultPlugin>(diagnostics.status());
    traits.diagnostics = diagnostics.take_value();
    traits.input_count = op.input_count;
    traits.repeated_minimum = op.repeated_minimum;
    traits.repeated_maximum = op.repeated_maximum;
    traits.repeated_match = !op.repeated_maximum || op.repeated_match != 0;
    traits.supports_cpu = (op.flags & PS_RESULT_FLAG_CPU_V5) != 0;
    traits.supports_gpu = (op.flags & PS_RESULT_FLAG_GPU_V5) != 0;
    traits.allows_cpu_fallback =
        (op.flags & PS_RESULT_FLAG_CPU_FALLBACK_V5) != 0;
    traits.deterministic = (op.flags & PS_RESULT_FLAG_DETERMINISTIC_V5) != 0;
    traits.side_effect_free =
        (op.flags & PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5) != 0;
    traits.cacheable = traits.deterministic && traits.side_effect_free;
    traits.share_blocks_across_outputs =
        (op.flags & PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V5) != 0;
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
        return Result<ImportedResultPlugin>(copied.status());
      traits.input_schema.push_back(copied.take_value());
      if (!declared.schema) {
        input_prototypes.emplace_back();
      } else {
        auto metadata = port(declared);
        if (!metadata.ok())
          return Result<ImportedResultPlugin>(metadata.status());
        input_prototypes.push_back(metadata.take_value());
      }
    }
    for (std::uint32_t output = 0; output < op.output_count; ++output) {
      const auto& declared = op.outputs[output];
      if (declared.struct_size != sizeof(declared) ||
          (declared.flags & ~(PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5)) ||
          (!(declared.flags & PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5) &&
           declared.maximum_output_payload_bytes) ||
          (declared.input_count != UINT32_MAX &&
           !array(declared.input_indices, declared.input_count, 1024)) ||
          (declared.input_count == UINT32_MAX && declared.input_indices))
        return Result<ImportedResultPlugin>(
            invalid("invalid named Result output"));
      auto name = text(declared.key, declared.key_size);
      if (!name.ok())
        return Result<ImportedResultPlugin>(name.status());
      auto metadata = port(declared.port);
      if (!metadata.ok())
        return Result<ImportedResultPlugin>(metadata.status());
      auto output_constraint = constraint(declared.port);
      if (!output_constraint.ok())
        return Result<ImportedResultPlugin>(output_constraint.status());
      OperationOutputTraits contract;
      contract.output_schema = output_constraint.take_value();
      contract.key = name.take_value();
      if (declared.flags & PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5)
        contract.maximum_output_payload_bytes =
            declared.maximum_output_payload_bytes;
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

      contract.output_schema.result_schema_id =
          std::string(metadata.value().result_schema->id);
      contract.output_schema.result_schema_version =
          metadata.value().result_schema->version;
      output_prototypes.push_back(metadata.take_value());
      traits.outputs.push_back(std::move(contract));
    }
    if (!array(op.shared_stages, op.shared_stage_count, 64))
      return Result<ImportedResultPlugin>(
          invalid("invalid shared stage table"));
    for (std::uint32_t i = 0; i < op.shared_stage_count; ++i) {
      auto stage = result_c::shared_stage(op.shared_stages[i]);
      if (!stage.ok())
        return Result<ImportedResultPlugin>(stage.status());
      traits.shared_stages.push_back(stage.take_value());
    }
    if (!array(op.parameters, op.parameter_count, 128))
      return Result<ImportedResultPlugin>(
          invalid("invalid Result parameter table"));
    for (std::uint32_t p = 0; p < op.parameter_count; ++p) {
      const auto& parameter = op.parameters[p];
      if (parameter.struct_size != sizeof(parameter) ||
          parameter.required > 1 || parameter.bounded > 1)
        return Result<ImportedResultPlugin>(
            invalid("invalid Result parameter"));
      auto name = text(parameter.key, parameter.key_size);
      if (!name.ok())
        return Result<ImportedResultPlugin>(name.status());
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
    auto owner = std::make_shared<Definition>(
        Definition{op, library, traits, std::move(input_prototypes),
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
    definition.start_result = [owner](const ResultProgramQuery& query,
                                      const BufferAllocator& allocator) {
      const auto bytes = query.shared_stage == UINT32_MAX
                             ? owner->api.state_bytes
                             : query.prepared->traits()
                                       .shared_stages.at(query.shared_stage)
                                       .continuation_bytes -
                                   c_member_storage_bytes();
      return start_c_member(owner, allocator, bytes);
    };
    definitions.operations.push_back(std::move(definition));
  }
  return Result<ImportedResultPlugin>(std::move(definitions));
}
}  // namespace ps::plugin_internal
