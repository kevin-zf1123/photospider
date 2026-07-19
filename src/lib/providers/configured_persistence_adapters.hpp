#pragma once

/**
 * @file configured_persistence_adapters.hpp
 * @brief Declares build-selected graph and cache persistence adapters.
 */

#include <memory>

#include "core/cache_metadata_codec.hpp"
#include "graph/graph_document_reader.hpp"
#include "graph/graph_document_writer.hpp"

namespace ps::providers {

/**
 * @brief Shared persistence owners selected by the product build.
 *
 * @throws Nothing for ordinary value moves and shared-owner destruction.
 * @note `document_reader` and `document_writer` may view the same concrete
 *       adapter control block. Every field is non-null when returned by
 *       `make_configured_persistence_adapters()`.
 */
struct ConfiguredPersistenceAdapters {
  /** @brief Cache metadata codec retained by GraphCacheService. */
  std::shared_ptr<const CacheMetadataCodec> metadata_codec;

  /** @brief Graph/node document reader retained by GraphIOService. */
  std::shared_ptr<const GraphDocumentReader> document_reader;

  /** @brief Graph/node document writer retained by GraphIOService. */
  std::shared_ptr<const GraphDocumentWriter> document_writer;
};

/**
 * @brief Constructs persistence adapters selected by build capabilities.
 *
 * @return Non-null metadata, document-reader, and document-writer owners.
 * @throws std::bad_alloc if adapter ownership allocation fails.
 * @note YAML-enabled products return the configured YAML implementations.
 *       YAML-disabled products return dependency-neutral unavailable adapters
 *       whose operations report `GraphErrc::Io`; they never report fake
 *       persistence success.
 */
ConfiguredPersistenceAdapters make_configured_persistence_adapters();

}  // namespace ps::providers
