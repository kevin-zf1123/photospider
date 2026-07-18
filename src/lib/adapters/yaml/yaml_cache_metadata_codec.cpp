#include "adapters/yaml/yaml_cache_metadata_codec.hpp"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <new>
#include <stdexcept>
#include <string>

#include "adapters/yaml/parameter_value_yaml.hpp"
#include "photospider/core/graph_error.hpp"

namespace ps::adapters::yaml {

/** @copydoc YamlCacheMetadataCodec::read */
plugin::ParameterMap YamlCacheMetadataCodec::read(
    const std::filesystem::path& path) const {
  try {
    const YAML::Node metadata = YAML::LoadFile(path.string());
    if (!metadata || metadata.IsNull()) {
      return {};
    }
    if (!metadata.IsMap()) {
      throw GraphError(GraphErrc::InvalidYaml,
                       "Disk cache metadata is not a map: " + path.string());
    }
    try {
      return internal::parameter_map_from_yaml(metadata);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::invalid_argument& error) {
      throw GraphError(
          GraphErrc::InvalidYaml,
          std::string("Invalid disk cache metadata: ") + error.what());
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const YAML::Exception& error) {
    throw GraphError(
        GraphErrc::InvalidYaml,
        std::string("Failed to parse disk cache metadata: ") + error.what());
  }
}

/** @copydoc YamlCacheMetadataCodec::write */
void YamlCacheMetadataCodec::write(const std::filesystem::path& path,
                                   const plugin::ParameterMap& values) const {
  try {
    const YAML::Node metadata = internal::parameter_map_to_yaml(values);
    std::ofstream output(path);
    if (!output.is_open()) {
      throw GraphError(
          GraphErrc::Io,
          "Failed to open disk cache metadata destination: " + path.string());
    }
    output << metadata;
    output.flush();
    if (!output) {
      throw GraphError(GraphErrc::Io,
                       "Failed to write disk cache metadata: " + path.string());
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const YAML::Exception& error) {
    throw GraphError(GraphErrc::Io, "Failed to emit disk cache metadata '" +
                                        path.string() + "': " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Io, "Failed to write disk cache metadata '" +
                                        path.string() + "': " + error.what());
  }
}

}  // namespace ps::adapters::yaml
