#include "providers/configured_persistence_adapters.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "photospider/core/graph_error.hpp"

#if defined(PHOTOSPIDER_HAS_YAML)
#include "adapters/yaml/yaml_cache_metadata_codec.hpp"
#include "adapters/yaml/yaml_graph_document_adapter.hpp"
#endif

namespace ps::providers {
namespace {

#if !defined(PHOTOSPIDER_HAS_YAML)

/**
 * @brief Reports unavailable graph-document persistence without parser types.
 *
 * @note The adapter owns no graph, parser, file, cache, or thread. It exists so
 *       YAML-disabled product composition retains complete non-null ownership
 *       and every representation-dependent operation fails explicitly.
 */
class UnavailableGraphDocumentAdapter final : public GraphDocumentReader,
                                              public GraphDocumentWriter {
 public:
  /** @copydoc GraphDocumentReader::read */
  GraphDefinition read(const std::filesystem::path& path) const override {
    throw GraphError(GraphErrc::Io,
                     "Graph document persistence is disabled for this build: " +
                         path.string());
  }

  /** @copydoc GraphDocumentReader::read_node */
  NodeDefinition read_node(const std::string& document_text) const override {
    (void)document_text;
    throw GraphError(
        GraphErrc::Io,
        "Graph node document persistence is disabled for this build");
  }

  /** @copydoc GraphDocumentWriter::write */
  void write(const std::filesystem::path& path,
             const GraphDefinition& definition) const override {
    (void)definition;
    throw GraphError(GraphErrc::Io,
                     "Graph document persistence is disabled for this build: " +
                         path.string());
  }

  /** @copydoc GraphDocumentWriter::write_node */
  std::string write_node(const NodeDefinition& definition) const override {
    (void)definition;
    throw GraphError(
        GraphErrc::Io,
        "Graph node document persistence is disabled for this build");
  }
};

/**
 * @brief Reports unavailable cache-metadata persistence without parser types.
 *
 * @note The adapter owns no cache entry, parser, file, retry, or transaction
 *       state. All calls fail before returning or publishing metadata.
 */
class UnavailableCacheMetadataCodec final : public CacheMetadataCodec {
 public:
  /** @copydoc CacheMetadataCodec::read */
  plugin::ParameterMap read(const std::filesystem::path& path) const override {
    throw GraphError(GraphErrc::Io,
                     "Cache metadata persistence is disabled for this build: " +
                         path.string());
  }

  /** @copydoc CacheMetadataCodec::write */
  void write(const std::filesystem::path& path,
             const plugin::ParameterMap& values) const override {
    (void)values;
    throw GraphError(GraphErrc::Io,
                     "Cache metadata persistence is disabled for this build: " +
                         path.string());
  }
};

#endif

}  // namespace

/** @copydoc make_configured_persistence_adapters */
ConfiguredPersistenceAdapters make_configured_persistence_adapters() {
#if defined(PHOTOSPIDER_HAS_YAML)
  auto metadata_codec =
      std::make_shared<adapters::yaml::YamlCacheMetadataCodec>();
  auto document_adapter =
      std::make_shared<adapters::yaml::YamlGraphDocumentAdapter>();
  return ConfiguredPersistenceAdapters{std::move(metadata_codec),
                                       document_adapter, document_adapter};
#else
  auto metadata_codec = std::make_shared<UnavailableCacheMetadataCodec>();
  auto document_adapter = std::make_shared<UnavailableGraphDocumentAdapter>();
  return ConfiguredPersistenceAdapters{std::move(metadata_codec),
                                       document_adapter, document_adapter};
#endif
}

}  // namespace ps::providers
