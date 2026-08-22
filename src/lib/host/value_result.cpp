#include "photospider/host/value_result.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

/**
 * @brief Validates one exact Host result name.
 * @param name Candidate output name.
 * @return Nothing.
 * @throws std::invalid_argument when the name is empty, oversized, or contains
 *         an embedded NUL byte.
 * @note Name validation reads no Value or graph state.
 */
void validate_result_name(const std::string& name) {
  if (name.empty() || name.size() > kMaximumHostResultNameBytes ||
      name.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "Host result name must contain 1-128 non-NUL bytes.");
  }
}

/**
 * @brief Validates optional portable metadata retained beside a Host Value.
 * @param metadata Candidate copied digest and statistics facts.
 * @return Nothing.
 * @throws std::invalid_argument or std::length_error for malformed facts.
 * @note Validation performs no digest computation, provider call, or payload
 *       access and therefore remains valid for non-Ready Values.
 */
void validate_portable_metadata(const NamedValuePortableMetadata& metadata) {
  const auto valid_digest_algorithm = [](const auto& digest) {
    return digest.algorithm == CanonicalDigestAlgorithm::Sha256CanonicalV1;
  };
  if ((metadata.descriptor_digest.has_value() &&
       !valid_digest_algorithm(*metadata.descriptor_digest)) ||
      (metadata.content_digest.has_value() &&
       !valid_digest_algorithm(*metadata.content_digest)) ||
      (metadata.storage_layout_digest.has_value() &&
       !valid_digest_algorithm(*metadata.storage_layout_digest))) {
    throw std::invalid_argument(
        "Host result portable metadata uses an unsupported digest.");
  }
  if (metadata.statistics_references.size() >
      kMaximumValueArtifactStatisticsReferences) {
    throw std::length_error(
        "Host result statistics-reference count exceeds its bound.");
  }
  for (const ValueArtifactStatisticsReference& reference :
       metadata.statistics_references) {
    const bool valid_text =
        !reference.algorithm.empty() &&
        reference.algorithm.size() <= kMaximumValueArtifactStringBytes &&
        reference.algorithm.find('\0') == std::string::npos &&
        !reference.artifact_identity.empty() &&
        reference.artifact_identity.size() <=
            kMaximumValueArtifactStringBytes &&
        reference.artifact_identity.find('\0') == std::string::npos;
    if (!metadata.content_digest.has_value() ||
        !(reference.content_digest == *metadata.content_digest) ||
        reference.content_digest.algorithm !=
            CanonicalDigestAlgorithm::Sha256CanonicalV1 ||
        reference.algorithm_version == 0U || !valid_text) {
      throw std::invalid_argument(
          "Host result statistics reference is malformed or foreign.");
    }
  }
}

/**
 * @brief Creates one payload-free buffer inspection record.
 * @param value Valid Value retaining the selected binding.
 * @param buffer_index Dense zero-based binding index.
 * @return Copied dependency-neutral binding facts.
 * @throws std::logic_error or std::out_of_range for inconsistent Value state.
 * @note Allocation and concrete ordinal are intentionally omitted.
 */
ValueBufferInspection inspect_buffer(const Value& value,
                                     std::size_t buffer_index) {
  const StorageBinding binding = value.storage_binding(buffer_index);
  return ValueBufferInspection{buffer_index, binding.device.backend(),
                               binding.memory_domain, binding.byte_size,
                               binding.host_visible};
}

/**
 * @brief Creates one complete payload-free named Value snapshot.
 * @param named Exact retained name and Value.
 * @return Owned inspection metadata.
 * @throws std::bad_alloc when copied metadata cannot allocate.
 * @note The only synchronization is one nonblocking ReadyFence poll.
 */
NamedValueInspection inspect_named_value(const NamedValue& named) {
  NamedValueInspection inspection;
  inspection.name = named.name;
  inspection.representation = named.value.representation_kind();
  inspection.layout_kind = named.value.storage_layout_kind();
  if (inspection.representation == ValueRepresentationKind::DenseTensor) {
    inspection.dense_descriptor = named.value.dense_tensor_descriptor();
    inspection.image_facet = named.value.image_facet();
    if (inspection.layout_kind == StorageLayoutKind::Strided) {
      inspection.strided_layout = named.value.strided_layout();
    } else if (inspection.layout_kind == StorageLayoutKind::Blocked) {
      inspection.blocked_layout = named.value.blocked_layout();
    } else {
      throw std::logic_error(
          "DenseTensor Value retains a provider-defined Layout.");
    }
  } else if (inspection.representation ==
             ValueRepresentationKind::ProviderDefined) {
    if (inspection.layout_kind != StorageLayoutKind::ProviderDefined) {
      throw std::logic_error(
          "Provider-defined Value retains a built-in Layout.");
    }
    inspection.provider_descriptor = named.value.provider_defined_descriptor();
    inspection.provider_layout = named.value.provider_defined_layout();
  } else {
    throw std::logic_error("Value retains an unknown representation kind.");
  }

  inspection.buffers.reserve(named.value.buffer_count());
  for (std::size_t index = 0U; index < named.value.buffer_count(); ++index) {
    inspection.buffers.push_back(inspect_buffer(named.value, index));
  }
  inspection.revision = named.value.revision_id();
  inspection.producer = named.value.producer_identity();
  inspection.descriptor_digest = named.portable_metadata.descriptor_digest;
  inspection.content_digest = named.portable_metadata.content_digest;
  inspection.storage_layout_digest =
      named.portable_metadata.storage_layout_digest;
  inspection.statistics_references =
      named.portable_metadata.statistics_references;
  const ReadyFenceSnapshot snapshot = named.value.ready_fence().poll();
  inspection.readiness = snapshot.state();
  if (snapshot.failure() != nullptr) {
    inspection.failure = *snapshot.failure();
  }
  return inspection;
}

}  // namespace

/** @copydoc NamedValueResult::NamedValueResult */
NamedValueResult::NamedValueResult(std::vector<NamedValue> values,
                                   bool require_ready) {
  if (values.size() > kMaximumHostResultValues) {
    throw std::length_error("Host result exceeds the named Value count bound.");
  }
  std::string previous;
  bool first = true;
  for (const NamedValue& named : values) {
    validate_result_name(named.name);
    if (!named.value.valid()) {
      throw std::invalid_argument("Host result contains an invalid Value.");
    }
    validate_portable_metadata(named.portable_metadata);
    if (!first && !(previous < named.name)) {
      throw std::invalid_argument(
          "Host result names must be strictly increasing and unique.");
    }
    if (require_ready && !named.value.ready_fence().poll().ready()) {
      throw std::invalid_argument(
          "Terminal Host result contains a non-Ready Value.");
    }
    previous = named.name;
    first = false;
  }
  values_ = std::move(values);
}

/** @copydoc NamedValueResult::find */
const Value* NamedValueResult::find(const std::string& name) const noexcept {
  const auto found = std::lower_bound(
      values_.begin(), values_.end(), name,
      [](const NamedValue& value, const std::string& candidate) {
        return value.name < candidate;
      });
  return found != values_.end() && found->name == name ? &found->value
                                                       : nullptr;
}

/** @copydoc NamedValueResult::inspect */
std::vector<NamedValueInspection> NamedValueResult::inspect() const {
  std::vector<NamedValueInspection> result;
  result.reserve(values_.size());
  for (const NamedValue& named : values_) {
    result.push_back(inspect_named_value(named));
  }
  return result;
}

}  // namespace ps
