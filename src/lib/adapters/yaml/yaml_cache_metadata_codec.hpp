#pragma once

#include "core/cache_metadata_codec.hpp"

namespace ps::adapters::yaml {

/**
 * @class YamlCacheMetadataCodec
 * @brief Persists detached cache metadata through the configured YAML schema.
 *
 * The adapter owns YAML nodes, `ParameterValue` conversion, stream emission,
 * and provider exception translation. Only paths, detached parameter maps,
 * `GraphError`, and `std::bad_alloc` cross its dependency-neutral contract.
 *
 * @note The adapter has no mutable state and is safe for independent calls
 * from different serialized graph-state lanes.
 */
class YamlCacheMetadataCodec final : public CacheMetadataCodec {
 public:
  /**
   * @brief Reads one YAML mapping from a cache metadata artifact.
   *
   * @param path Existing `.yml` metadata path.
   * @return Detached map; null or empty YAML documents produce an empty map.
   * @throws std::bad_alloc when parser or value allocation is exhausted.
   * @throws GraphError with `GraphErrc::InvalidYaml` for malformed YAML,
   * non-map roots, unsupported values, or normalized-key collisions.
   * @note Numeric mapping keys retain the configured YAML adapter's canonical
   * normalization and collision rules.
   */
  plugin::ParameterMap read(const std::filesystem::path& path) const override;

  /**
   * @brief Writes one detached map using the existing YAML metadata schema.
   *
   * @param path Destination `.yml` metadata path prepared by cache policy.
   * @param values Named values to serialize synchronously.
   * @return Nothing.
   * @throws std::bad_alloc when YAML or stream allocation is exhausted.
   * @throws GraphError with `GraphErrc::Io` when values cannot be emitted or
   * the destination cannot be opened, written, or flushed.
   * @note The adapter does not create parent directories or alter cache
   * visibility; `GraphCacheService` owns those decisions.
   */
  void write(const std::filesystem::path& path,
             const plugin::ParameterMap& values) const override;
};

}  // namespace ps::adapters::yaml
