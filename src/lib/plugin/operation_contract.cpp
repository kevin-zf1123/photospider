#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/core/checked_math.hpp"
#include "photospider/core/status_helpers.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/port_validation.hpp"

namespace ps {
Result<OperationTraits> select_operation_output(const OperationTraits& traits,
                                                std::uint32_t output_index) {
  if (output_index >= traits.outputs.size())
    return Result<OperationTraits>(
        core_internal::invalid_schema_domain("unknown output index"));
  auto selected = traits;
  selected.outputs = {traits.outputs[output_index]};
  return Result<OperationTraits>(std::move(selected));
}

Result<OperationTraits> resolve_operation_traits(
    const OperationTraits& traits, std::size_t count,
    const std::map<std::string, ParameterValue>& parameters) {
  if (traits.outputs.empty() || traits.outputs.size() > 64)
    return Result<OperationTraits>(
        core_internal::invalid_schema_domain("invalid output count"));
  auto status = validate_operation_parameters(traits, parameters);
  if (!status.ok())
    return Result<OperationTraits>(status);
  auto result = traits;
  if (std::any_of(traits.outputs.begin(), traits.outputs.end(),
                  [](const auto& output) { return !output.result_schema; }) ||
      count > 1024 || traits.version != 28)
    return Result<OperationTraits>(core_internal::invalid_schema_domain(
        "invalid operation version/count"));
  if (traits.repeated_maximum && !traits.repeated_resolved) {
    if (traits.input_schema.size() != traits.input_count + 1 ||
        count < traits.input_count ||
        count - traits.input_count < traits.repeated_minimum ||
        count - traits.input_count > traits.repeated_maximum)
      return Result<OperationTraits>(core_internal::invalid_schema_domain(
          "repeated input count outside bounds"));
    result.repeated_resolved =
        static_cast<std::uint32_t>(count - traits.input_count);
    result.input_schema.resize(count, traits.input_schema.back());
    result.input_count = static_cast<std::uint32_t>(count);
  } else if (count != traits.input_count ||
             traits.input_schema.size() != count) {
    return Result<OperationTraits>(
        core_internal::invalid_schema_domain("operation input count mismatch"));
  }
  return Result<OperationTraits>(std::move(result));
}

Result<OperationMetadata> infer_operation_output(
    const OperationTraits& t, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>&) {
  if (t.requires_metadata_specialization)
    return Result<OperationMetadata>(core_internal::invalid_schema_domain(
        "operation template requires registry metadata specialization"));
  if (t.version != 28 || t.outputs.size() != 1 || !t.outputs[0].result_schema)
    return Result<OperationMetadata>(core_internal::invalid_schema_domain(
        "select one output for singleton inference"));
  const auto mismatch = [](const char* message) {
    return Result<OperationMetadata>(
        Status{ErrorCode::TypeMismatch,
               message,
               FailureReason::None,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  if (inputs.size() != t.input_count || inputs.size() != t.input_schema.size())
    return mismatch("inference input count mismatch");
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    auto status =
        input_internal::validate_port_metadata(t.input_schema[i], inputs[i]);
    if (!status.ok()) {
      if (status.detail.origin == FailureOrigin::Unspecified &&
          (status.code == ErrorCode::TypeMismatch ||
           status.code == ErrorCode::InvalidArgument)) {
        status.detail.origin = FailureOrigin::Schema;
        if (status.code == ErrorCode::InvalidArgument &&
            status.reason == FailureReason::None)
          status.reason = FailureReason::InvalidDomain;
      }
      return Result<OperationMetadata>(status);
    }
  }
  if (t.repeated_resolved && t.repeated_match) {
    if (t.repeated_resolved > inputs.size())
      return mismatch("invalid resolved repetition");
    const auto first = inputs.size() - t.repeated_resolved;
    const auto descriptor = [&](std::size_t i) -> Result<TensorDescriptor> {
      auto slot =
          input_internal::resolve_tensor_member(t.input_schema[i], inputs[i]);
      if (!slot.ok())
        return Result<TensorDescriptor>(slot.status());
      const auto& tensor = inputs[i].result_schema->tensors[slot.value()];
      return Result<TensorDescriptor>(TensorDescriptor{
          tensor.descriptor.element_type, tensor.sample_shape()});
    };
    auto expected = descriptor(first);
    if (!expected.ok())
      return Result<OperationMetadata>(expected.status());
    for (std::size_t i = first + 1; i < inputs.size(); ++i) {
      auto actual = descriptor(i);
      if (!actual.ok())
        return Result<OperationMetadata>(actual.status());
      if (actual.value().shape != expected.value().shape ||
          actual.value().element_type != expected.value().element_type)
        return mismatch("homogeneous repeated input descriptors differ");
    }
  }
  OperationMetadata metadata;
  metadata.result_schema =
      std::make_shared<const SchemaTemplate>(*t.outputs[0].result_schema);
  auto valid = input_internal::validate_port_metadata(
      t.outputs[0].output_schema, metadata);
  return valid.ok() ? Result<OperationMetadata>(std::move(metadata))
                    : Result<OperationMetadata>(valid);
}
Result<std::vector<OperationMetadata>> infer_operation_outputs(
    const OperationTraits& traits, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  if (traits.outputs.empty() || traits.outputs.size() > 64)
    return Result<std::vector<OperationMetadata>>(
        core_internal::invalid_schema_domain("invalid output count"));
  std::vector<OperationMetadata> results;
  for (std::uint32_t i = 0; i < traits.outputs.size(); ++i) {
    auto selected = select_operation_output(traits, i);
    auto output = infer_operation_output(selected.value(), inputs, parameters);
    if (!output.ok())
      return Result<std::vector<OperationMetadata>>(output.status());
    results.push_back(output.take_value());
  }
  return Result<std::vector<OperationMetadata>>(std::move(results));
}
namespace input_internal {
Status validate_operation_contract(const OperationTraits& t) {
  if (t.outputs.size() != 1)
    return core_internal::invalid_schema_domain("select one output contract");
  const auto& selected = t.outputs[0];
  if (!selected.result_schema)
    return core_internal::invalid_schema_domain(
        "Result programs require Result input and output schemas");
  if (t.cpu_staged_tiles && (!t.supports_cpu || t.supports_gpu))
    return core_internal::invalid_schema_domain(
        "CPU stages require a CPU-only continuation");
  if (!selected.continuation_bytes || !selected.maximum_dependency_stages ||
      selected.maximum_dependency_stages > 1048576)
    return core_internal::invalid_schema_domain(
        "invalid dependency phase contract");
  if (t.shared_stages.size() > 64)
    return core_internal::invalid_schema_domain("too many shared stages");
  bool mandatory = false;
  for (std::size_t i = 0; i < t.shared_stages.size(); ++i) {
    const auto& stage = t.shared_stages[i];
    if (stage.key.empty() || stage.key.size() > 128 ||
        !stage.result_schema.validate().ok() || !stage.continuation_bytes ||
        !stage.maximum_work || !stage.maximum_dependency_stages ||
        stage.maximum_dependency_stages > 1048576 ||
        stage.input_indices.size() > t.input_count ||
        (stage.backend != Backend::Cpu && stage.backend != Backend::Gpu) ||
        (stage.backend == Backend::Cpu && !t.supports_cpu) ||
        (stage.backend == Backend::Gpu && !t.supports_gpu))
      return core_internal::invalid_schema_domain(
          "invalid shared stage contract");
    mandatory |= stage.mandatory;
    for (std::size_t j = 0; j < i; ++j)
      if (t.shared_stages[j].key == stage.key)
        return core_internal::invalid_schema_domain(
            "duplicate shared stage key");
    for (std::size_t j = 0; j < stage.input_indices.size(); ++j) {
      const auto port = stage.input_indices[j];
      if (port >= t.input_count || (j && stage.input_indices[j - 1] >= port) ||
          (stage.mandatory && selected.input_indices &&
           std::find(selected.input_indices->begin(),
                     selected.input_indices->end(),
                     port) == selected.input_indices->end()))
        return core_internal::invalid_schema_domain(
            "invalid stage projection or mandatory stage outside output "
            "declaration");
    }
  }
  if (!t.side_effect_free && !mandatory && !t.requires_metadata_specialization)
    return core_internal::invalid_schema_domain(
        "nonpure operations require a mandatory shared stage");
  const auto& output = t.outputs[0];
  if (!output.result_schema)
    return core_internal::invalid_schema_domain(
        "structured output requires a complete schema template");
  if (output.result_schema &&
      (!output.result_schema->validate().ok() ||
       (output.output_schema.result_schema_id.empty()
            ? (!t.requires_metadata_specialization ||
               !tensor_member_predicate(output.output_schema))
            : (std::string_view(output.result_schema->id) !=
                   std::string_view(output.output_schema.result_schema_id) ||
               output.result_schema->version !=
                   output.output_schema.result_schema_version))))
    return core_internal::invalid_schema_domain(
        "invalid structured output template");
  if (t.repeated_resolved || t.repeated_maximum > 1024 ||
      t.repeated_minimum > t.repeated_maximum ||
      (t.repeated_maximum &&
       (!t.repeated_minimum ||
        (!t.repeated_match && !t.requires_metadata_specialization) ||
        !core_internal::can_add(t.input_count, t.repeated_maximum, 1024))) ||
      (!t.repeated_maximum && t.repeated_minimum))
    return core_internal::invalid_schema_domain(
        "invalid repeated input template");
  return Status::success();
}
}  // namespace input_internal
}  // namespace ps
