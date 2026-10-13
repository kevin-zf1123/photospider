#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "core/status_helpers.hpp"
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
      std::any_of(traits.input_schema.begin(), traits.input_schema.end(),
                  [](const auto& input) {
                    return input.kind != OperationPortKind::Result;
                  }) ||
      count > 1024 || traits.version != 25)
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
  if (t.version != 25 || t.outputs.size() != 1 || !t.outputs[0].result_schema)
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
    const auto descriptor = [&](std::size_t i) -> Result<ValueDescriptor> {
      auto slot =
          input_internal::resolve_tensor_member(t.input_schema[i], inputs[i]);
      if (!slot.ok())
        return Result<ValueDescriptor>(slot.status());
      const auto& tensor = inputs[i].result_schema->tensors[slot.value()];
      return Result<ValueDescriptor>(ValueDescriptor{
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
  std::vector<std::vector<std::uint64_t>> domains;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& input = inputs[i];
    if (!input.result_schema->tensors.empty()) {
      if (t.input_schema[i].tensor_key.empty() &&
          input.result_schema->tensors.size() != 1) {
        domains.emplace_back();
        continue;
      }
      auto slot =
          input_internal::resolve_tensor_member(t.input_schema[i], input);
      if (!slot.ok())
        return Result<OperationMetadata>(slot.status());
      domains.push_back(
          input.result_schema->tensors[slot.value()].sample_shape());
      continue;
    }
    std::vector<std::uint64_t> domain;
    for (const auto& extent : input.result_schema->domain) {
      if (extent.kind != ResultExtentKind::Fixed) {
        domain.clear();
        break;
      }
      const auto n =
          extent.value / extent.divisor + (extent.value % extent.divisor != 0);
      if (!core_internal::can_add(n, extent.offset))
        return mismatch("result domain overflows");
      domain.push_back(n + extent.offset);
    }
    domains.push_back(std::move(domain));
  }
  auto schema = t.outputs[0].result_schema->resolve(domains);
  if (!schema.ok())
    return Result<OperationMetadata>(schema.status());
  OperationMetadata metadata;
  metadata.result_schema =
      std::make_shared<const SchemaTemplate>(schema.take_value());
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
  if (!selected.result_schema ||
      std::any_of(t.input_schema.begin(), t.input_schema.end(),
                  [](const auto& input) {
                    return input.kind != OperationPortKind::Result;
                  }))
    return core_internal::invalid_schema_domain(
        "Result programs require Result input and output schemas");
  if (selected.data_movement != DataMovementKind::None &&
      selected.data_movement != DataMovementKind::BitwiseMapped)
    return core_internal::invalid_schema_domain("unknown data movement kind");
  const bool tiled = selected.region_rule == OperationRegionRule::Tiled;
  const bool whole = selected.region_rule == OperationRegionRule::Whole;
  if (t.cpu_staged_tiles &&
      (!t.supports_cpu || t.supports_gpu ||
       (selected.region_rule != OperationRegionRule::Whole &&
        selected.region_rule != OperationRegionRule::Tiled)))
    return core_internal::invalid_schema_domain(
        "CPU stages require a CPU-only Result continuation");
  if (selected.preserve_output_views &&
      (!t.supports_cpu || t.supports_gpu || !(tiled || whole)))
    return core_internal::invalid_schema_domain(
        "view output requires CPU execution");
  if (selected.requires_input_views &&
      (!whole || !selected.preserve_output_views))
    return core_internal::invalid_schema_domain(
        "original input views require CPU Whole view output");
  if (t.outputs[0].maximum_output_payload_bytes &&
      (!t.supports_cpu || t.supports_gpu || !(tiled || whole)))
    return core_internal::invalid_schema_domain(
        "explicit output payload bound requires a CPU Whole or staged view");

  const bool whole_result =
      whole && selected.failure_delivery == FailureDelivery::RequestFailureOnly;
  if (!whole_result && (!t.deterministic || !t.side_effect_free))
    return core_internal::invalid_schema_domain(
        "regional programs require deterministic side-effect-free behavior");
  if (t.outputs[0].failure_delivery != FailureDelivery::RequestFailureOnly ||
      (!whole && !tiled) || !selected.continuation_bytes ||
      !selected.maximum_dependency_stages ||
      selected.maximum_dependency_stages > 1048576)
    return core_internal::invalid_schema_domain(
        "invalid dependency phase contract");
  const auto& output = t.outputs[0];
  if (output.result_schema.has_value() !=
      (output.output_schema.kind == OperationPortKind::Result))
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
