#pragma once

#include <cstring>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::contract_internal {
/** @brief Canonical full facets shared by stage and derived-result identities.
 */
template <class Digest>
void append_facets(Digest* digest,
                   const std::vector<ValueFacet>& facets) noexcept {
  digest->integer(facets.size());
  for (const auto& f : facets) {
    digest->text(f.key);
    digest->integer(f.version);
    digest->integer(f.payload.size());
    digest->bytes(f.payload.data(), f.payload.size());
  }
}
/** @brief Appends the exact binary32 port contract as uint64 fields. */
template <class Digest>
void append_port(Digest* digest, const OperationPortConstraint& port) noexcept {
  digest->integer(static_cast<std::uint32_t>(port.kind));
  std::uint32_t minimum = 0, maximum = 0;
  std::memcpy(&minimum, &port.minimum, sizeof(minimum));
  std::memcpy(&maximum, &port.maximum, sizeof(maximum));
  digest->integer(minimum);
  digest->integer(maximum);
  digest->integer(port.semantic_kind);
  digest->integer(port.element_type);
  digest->integer(port.rank);
  digest->integer(port.element_type_mask);
  append_facets(digest, port.facets);
}

/**
 * @brief Appends compiler-visible operation traits.
 * @param digest Destination builder.
 * @param traits Copied semantic traits.
 * @throws Nothing.
 * @note Callback/library identity is intentionally excluded.
 */
template <class Digest>
void append_traits(Digest* digest, const OperationTraits& traits) {
  digest->integer(traits.outputs.size());
  if (traits.outputs.size() != 1) {
    for (const auto& output : traits.outputs) {
      // Preserve registry order, including the full contract of every result.
      auto selected = traits;
      selected.outputs = {output};
      append_traits(digest, selected);
    }
    return;
  }
  const auto& output = traits.outputs[0];
  digest->text(output.key);
  digest->integer(output.input_indices.has_value());
  if (output.input_indices) {
    digest->integer(output.input_indices->size());
    for (auto port : *output.input_indices)
      digest->integer(port);
  }
  digest->integer(traits.input_count);
  digest->integer(traits.input_schema.size());
  for (const auto& port : traits.input_schema)
    append_port(digest, port);
  append_port(digest, traits.outputs[0].output_schema);
  digest->integer(traits.deterministic ? 1U : 0U);
  digest->integer(traits.side_effect_free ? 1U : 0U);
  digest->integer(traits.supports_cpu ? 1U : 0U);
  digest->integer(traits.supports_gpu ? 1U : 0U);
  digest->integer(traits.allows_cpu_fallback ? 1U : 0U);
  digest->integer(traits.estimated_bytes);
  digest->integer(traits.workspace_bytes);
  digest->integer(traits.workspace_input_multiplier);
  digest->integer(traits.version);
  digest->integer(traits.cacheable ? 1U : 0U);
  digest->integer(
      static_cast<std::uint32_t>(traits.outputs[0].output_element_type));
  digest->integer(static_cast<std::uint32_t>(traits.outputs[0].shape_rule));
  digest->integer(static_cast<std::uint32_t>(traits.outputs[0].region_rule));
  digest->integer(traits.outputs[0].halo_radius);
  digest->text(traits.outputs[0].halo_radius_parameter);
  digest->text(traits.outputs[0].spatial_factor_parameter);
  digest->integer(traits.outputs[0].spatial_factor);
  digest->integer(traits.parameter_schema.size());
  for (const OperationParameterSpec& parameter : traits.parameter_schema) {
    digest->text(parameter.key);
    digest->integer(static_cast<std::uint32_t>(parameter.type));
    digest->integer(parameter.required ? 1U : 0U);
    digest->integer(parameter.bounded ? 1U : 0U);
    std::uint64_t minimum = 0, maximum = 0;
    std::memcpy(&minimum, &parameter.minimum, sizeof(minimum));
    std::memcpy(&maximum, &parameter.maximum, sizeof(maximum));
    digest->integer(minimum);
    digest->integer(maximum);
  }
  digest->integer(traits.outputs[0].fixed_output_shape.size());
  for (std::uint64_t extent : traits.outputs[0].fixed_output_shape) {
    digest->integer(extent);
  }
  digest->integer(
      static_cast<std::uint32_t>(traits.outputs[0].output_dtype_rule));
  digest->integer(traits.outputs[0].output_dtype_input);
  digest->text(traits.outputs[0].output_dtype_parameter);
  digest->integer(traits.outputs[0].output_axes.size());
  for (const auto& axis : traits.outputs[0].output_axes) {
    digest->integer(static_cast<std::uint32_t>(axis.source));
    digest->integer(axis.constant);
    digest->text(axis.parameter);
    digest->integer(axis.input);
    digest->integer(axis.axis);
    digest->integer(axis.offset);
    digest->text(axis.subtract_parameter);
    digest->integer(axis.divisor);
    digest->integer(axis.multiplier);
  }
  digest->integer(traits.repeated_minimum);
  digest->integer(traits.repeated_maximum);
  digest->integer(traits.repeated_resolved);
  digest->integer(traits.repeated_match);
  digest->integer(
      static_cast<std::uint32_t>(traits.outputs[0].output_semantic_rule));
  digest->integer(traits.outputs[0].output_semantic_input);
  append_facets(digest, traits.outputs[0].output_facets);
  digest->text(traits.outputs[0].output_semantic_parameter);
  digest->integer(traits.outputs[0].requires_dense_output);
  digest->integer(
      static_cast<std::uint32_t>(traits.outputs[0].observation_kind));
  digest->integer(
      static_cast<std::uint32_t>(traits.outputs[0].failure_delivery));
  digest->integer(traits.outputs[0].dependency_version);
  digest->integer(traits.outputs[0].continuation_bytes);
  digest->integer(traits.outputs[0].maximum_dependency_stages);
}

}  // namespace ps::contract_internal
