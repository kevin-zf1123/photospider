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
void append_traits(Digest* digest, const OperationTraits& traits) noexcept {
  digest->integer(traits.input_count);
  digest->integer(traits.input_schema.size());
  for (const auto& port : traits.input_schema)
    append_port(digest, port);
  append_port(digest, traits.output_schema);
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
  digest->integer(static_cast<std::uint32_t>(traits.output_element_type));
  digest->integer(static_cast<std::uint32_t>(traits.shape_rule));
  digest->integer(static_cast<std::uint32_t>(traits.region_rule));
  digest->integer(traits.halo_radius);
  digest->text(traits.halo_radius_parameter);
  digest->text(traits.spatial_factor_parameter);
  digest->integer(traits.spatial_factor);
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
  digest->integer(traits.fixed_output_shape.size());
  for (std::uint64_t extent : traits.fixed_output_shape) {
    digest->integer(extent);
  }
  digest->integer(static_cast<std::uint32_t>(traits.output_dtype_rule));
  digest->integer(traits.output_dtype_input);
  digest->text(traits.output_dtype_parameter);
  digest->integer(traits.output_axes.size());
  for (const auto& axis : traits.output_axes) {
    digest->integer(static_cast<std::uint32_t>(axis.source));
    digest->integer(axis.constant);
    digest->text(axis.parameter);
    digest->integer(axis.input);
    digest->integer(axis.axis);
    digest->integer(axis.offset);
  }
  digest->integer(traits.repeated_minimum);
  digest->integer(traits.repeated_maximum);
  digest->integer(traits.repeated_resolved);
  digest->integer(traits.repeated_match);
  digest->integer(static_cast<std::uint32_t>(traits.output_semantic_rule));
  digest->integer(traits.output_semantic_input);
  append_facets(digest, traits.output_facets);
  digest->text(traits.output_semantic_parameter);
  digest->integer(traits.requires_dense_output);
}

}  // namespace ps::contract_internal
