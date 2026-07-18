#pragma once

/**
 * @file cache_test_dependencies.hpp
 * @brief Supplies configured cache-metadata dependencies to backend tests.
 */

#include <memory>

#include "adapters/yaml/yaml_cache_metadata_codec.hpp"  // NOLINT(build/include_subdir)

namespace ps::testing {

/**
 * @brief Creates one configured YAML cache-metadata codec owner.
 *
 * @return Mutable concrete owner convertible to the immutable codec contract.
 * @throws std::bad_alloc if shared ownership allocation fails.
 * @note The returned stateless instance is isolated per test and owns no cache
 * policy, path derivation, diagnostic, or graph state.
 */
inline std::shared_ptr<adapters::yaml::YamlCacheMetadataCodec>
make_yaml_cache_metadata_codec() {
  return std::make_shared<adapters::yaml::YamlCacheMetadataCodec>();
}

}  // namespace ps::testing
