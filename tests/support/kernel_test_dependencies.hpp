#pragma once

/**
 * @file kernel_test_dependencies.hpp
 * @brief Supplies explicit configured dependencies to direct Kernel tests.
 */

#include <memory>
#include <utility>

#include "core/image_artifact_codec.hpp"  // NOLINT(build/include_subdir)
#include "providers/configured_image_artifact_codec.hpp"  // NOLINT(build/include_subdir)
#include "runtime/kernel.hpp"  // NOLINT(build/include_subdir)
#include "support/cache_test_dependencies.hpp"
#include "support/graph_document_test_dependencies.hpp"

namespace ps::testing {

/**
 * @brief Creates a direct-test Kernel with configured product dependencies.
 *
 * @return Fresh Kernel retaining one YAML adapter as both document contracts.
 * @throws std::bad_alloc if configured dependency ownership allocation fails.
 * @throws std::system_error if configured codec initialization synchronization
 *         fails.
 * @note C++17 guaranteed copy elision constructs the non-copyable Kernel at
 *       the caller. Product code still composes through `create_embedded_host`.
 */
inline Kernel make_kernel_with_yaml_graph_documents() {
  auto adapter = make_yaml_graph_document_adapter();
  return Kernel(providers::make_configured_image_artifact_codec(),
                make_yaml_cache_metadata_codec(), adapter, adapter);
}

/**
 * @brief Creates a direct-test Kernel with an explicit artifact codec.
 *
 * @param image_codec Shared codec owner retained by GraphCacheService.
 * @return Fresh Kernel using the codec and one configured YAML adapter.
 * @throws std::invalid_argument when `image_codec` is empty.
 * @throws std::bad_alloc if graph-document ownership allocation fails.
 * @note This overload isolates cache-codec tests while keeping persistence
 *       composition explicit.
 */
inline Kernel make_kernel_with_yaml_graph_documents(
    std::shared_ptr<const ImageArtifactCodec> image_codec) {
  auto adapter = make_yaml_graph_document_adapter();
  return Kernel(std::move(image_codec), make_yaml_cache_metadata_codec(),
                adapter, adapter);
}

/**
 * @brief Creates a direct-test Kernel with explicit cache artifact codecs.
 *
 * @param image_codec Shared image codec retained by GraphCacheService.
 * @param metadata_codec Shared metadata codec retained by GraphCacheService.
 * @return Fresh Kernel using both codecs and one configured YAML document
 * adapter.
 * @throws std::invalid_argument when either codec is empty.
 * @throws std::bad_alloc if graph-document ownership allocation fails.
 * @note This overload supports metadata lifetime/error substitution without a
 * compatibility default inside runtime code.
 */
inline Kernel make_kernel_with_yaml_graph_documents(
    std::shared_ptr<const ImageArtifactCodec> image_codec,
    std::shared_ptr<const CacheMetadataCodec> metadata_codec) {
  auto adapter = make_yaml_graph_document_adapter();
  return Kernel(std::move(image_codec), std::move(metadata_codec), adapter,
                adapter);
}

/**
 * @brief Allocates a direct-test Kernel with an explicit artifact codec.
 *
 * @param image_codec Shared codec owner retained by GraphCacheService.
 * @return Unique Kernel owner using one configured YAML adapter.
 * @throws std::invalid_argument when `image_codec` is empty.
 * @throws std::bad_alloc if adapter or Kernel ownership allocation fails.
 * @note This form supports teardown tests that transfer or asynchronously
 *       destroy the unique Kernel owner.
 */
inline std::unique_ptr<Kernel> make_unique_kernel_with_yaml_graph_documents(
    std::shared_ptr<const ImageArtifactCodec> image_codec) {
  auto adapter = make_yaml_graph_document_adapter();
  return std::make_unique<Kernel>(std::move(image_codec),
                                  make_yaml_cache_metadata_codec(), adapter,
                                  adapter);
}

/**
 * @brief Allocates a direct-test Kernel with explicit cache artifact codecs.
 *
 * @param image_codec Shared image codec retained by GraphCacheService.
 * @param metadata_codec Shared metadata codec retained by GraphCacheService.
 * @return Unique Kernel owner using both codecs and one YAML document adapter.
 * @throws std::invalid_argument when either codec is empty.
 * @throws std::bad_alloc if adapter or Kernel ownership allocation fails.
 * @note This form supports teardown tests that observe either retained codec.
 */
inline std::unique_ptr<Kernel> make_unique_kernel_with_yaml_graph_documents(
    std::shared_ptr<const ImageArtifactCodec> image_codec,
    std::shared_ptr<const CacheMetadataCodec> metadata_codec) {
  auto adapter = make_yaml_graph_document_adapter();
  return std::make_unique<Kernel>(std::move(image_codec),
                                  std::move(metadata_codec), adapter, adapter);
}

}  // namespace ps::testing
