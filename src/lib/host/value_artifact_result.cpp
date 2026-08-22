#include "photospider/host/value_artifact_result.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace ps {

/** @copydoc capture_named_value_artifact_set */
NamedValueArtifactSet capture_named_value_artifact_set(
    const NamedValueResult& result) {
  NamedValueArtifactSet artifacts;
  artifacts.values.reserve(result.values().size());
  for (const NamedValue& named : result.values()) {
    ValueArtifact artifact = capture_value_artifact(named.name, named.value);
    const NamedValuePortableMetadata& metadata = named.portable_metadata;
    if ((metadata.descriptor_digest.has_value() &&
         !(*metadata.descriptor_digest ==
           artifact.envelope.descriptor_digest)) ||
        (metadata.content_digest.has_value() &&
         (!artifact.envelope.content_digest.has_value() ||
          !(*metadata.content_digest == *artifact.envelope.content_digest))) ||
        (metadata.storage_layout_digest.has_value() &&
         !(*metadata.storage_layout_digest ==
           artifact.envelope.storage_layout_digest))) {
      throw std::invalid_argument(
          "Host result portable digest disagrees with captured Value.");
    }
    artifact.envelope.statistics_references = metadata.statistics_references;
    validate_value_artifact(artifact);
    artifacts.values.push_back(std::move(artifact));
  }
  return artifacts;
}

/** @copydoc reconstruct_named_value_artifact_set */
NamedValueResult reconstruct_named_value_artifact_set(
    const NamedValueArtifactSet& artifacts, DataDefinitionRegistry* registry) {
  std::vector<NamedValue> values;
  values.reserve(artifacts.values.size());
  for (const ValueArtifact& artifact : artifacts.values) {
    NamedValue named;
    named.name = artifact.envelope.output_name;
    named.value = reconstruct_value_artifact(artifact, registry);
    named.portable_metadata.descriptor_digest =
        artifact.envelope.descriptor_digest;
    named.portable_metadata.content_digest = artifact.envelope.content_digest;
    named.portable_metadata.storage_layout_digest =
        artifact.envelope.storage_layout_digest;
    named.portable_metadata.statistics_references =
        artifact.envelope.statistics_references;
    values.push_back(std::move(named));
  }
  return NamedValueResult(std::move(values));
}

}  // namespace ps
