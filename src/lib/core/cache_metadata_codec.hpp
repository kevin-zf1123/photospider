#pragma once

#include <filesystem>

#include "photospider/plugin/node_view.hpp"

namespace ps {

/**
 * @class CacheMetadataCodec
 * @brief Dependency-neutral boundary for disk-cache metadata persistence.
 *
 * Implementations translate between one kernel-selected filesystem artifact
 * and a detached `plugin::ParameterMap`. The interface deliberately exposes no
 * parser/provider type and owns no cache-entry selection, path derivation,
 * directory creation, timing, retry, diagnostic, HP-authority, or stale-file
 * synchronization policy.
 *
 * @note Implementations must be safe for independent calls from serialized
 * graph-state lanes belonging to different graphs. Callers retain an immutable
 * shared owner for the complete cache-service lifetime.
 */
class CacheMetadataCodec {
 public:
  /**
   * @brief Destroys one codec after every retaining service releases it.
   * @throws Nothing.
   * @note Destruction must not publish caller-visible cache state.
   */
  virtual ~CacheMetadataCodec() = default;

  /**
   * @brief Reads one metadata artifact into detached named values.
   *
   * @param path Existing metadata path selected by the cache owner.
   * @return Deep-owned named values; an implementation may return an empty map
   * for a valid empty representation.
   * @throws std::bad_alloc when path, parser, or value allocation is exhausted.
   * @throws GraphError with an implementation-normalized category when the
   * artifact cannot be read or represented.
   * @note The returned map must not alias parser or adapter-local storage.
   */
  virtual plugin::ParameterMap read(
      const std::filesystem::path& path) const = 0;

  /**
   * @brief Writes detached named values to one metadata artifact.
   *
   * @param path Destination metadata path prepared by the cache owner.
   * @param values Borrowed named values that remain alive for this synchronous
   * call.
   * @return Nothing.
   * @throws std::bad_alloc when path, emitter, or output allocation is
   * exhausted.
   * @throws GraphError with an implementation-normalized category when values
   * cannot be represented or the destination cannot be written.
   * @note The method does not own directory creation, atomic replacement,
   * retry, cache visibility, or deletion policy.
   */
  virtual void write(const std::filesystem::path& path,
                     const plugin::ParameterMap& values) const = 0;
};

}  // namespace ps
