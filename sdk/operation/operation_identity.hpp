#pragma once

#include <cstring>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::contract_internal {
template <class Digest>
void append_extensions(Digest* digest,
                       const std::vector<ExtensionIdentity>& references) {
  digest->integer(references.size());
  for (const auto& reference : references) {
    digest->text(reference.key);
    digest->integer(reference.version);
    digest->text(reference.implementation);
  }
}
/** @brief Appends the structural port contract as uint64 fields. */
template <class Digest>
void append_port(Digest* digest, const OperationPortConstraint& port) noexcept {
  digest->integer(port.element_type);
  digest->integer(port.rank);
  digest->integer(port.element_type_mask);
  digest->text(port.result_schema_id);
  digest->integer(port.result_schema_version);
  digest->text(port.tensor_key);
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
  append_extensions(digest, traits.diagnostics);
  digest->integer(traits.requires_metadata_specialization);

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
  digest->integer(output.result_schema.has_value());
  if (output.result_schema)
    digest->text(output.result_schema->canonical());
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
  digest->integer(traits.version);
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
  digest->integer(traits.repeated_minimum);
  digest->integer(traits.repeated_maximum);
  digest->integer(traits.repeated_resolved);
  digest->integer(traits.repeated_match);
}

}  // namespace ps::contract_internal
