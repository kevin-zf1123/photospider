#pragma once

/**
 * @file embedded_host_dependencies.hpp
 * @brief Declares the private explicit-dependency embedded Host root.
 */

#include <memory>

#include "core/cache_metadata_codec.hpp"    // NOLINT(build/include_subdir)
#include "core/image_artifact_codec.hpp"    // NOLINT(build/include_subdir)
#include "graph/graph_document_reader.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_document_writer.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

namespace ps::internal {

/**
 * @brief Creates an embedded Host with explicit persistence dependencies.
 *
 * @param image_codec Shared artifact codec owner retained by Kernel cache IO.
 * @param metadata_codec Shared metadata codec owner retained by Kernel cache
 * IO.
 * @param document_reader Shared graph/node document reader owner.
 * @param document_writer Shared graph/node document writer owner.
 * @return Fresh embedded Host using exactly the supplied dependencies.
 * @throws std::invalid_argument when any required owner is empty.
 * @throws std::bad_alloc if Host/backend ownership allocation fails.
 * @note This private, non-installed composition seam exists for durable
 *       substitution tests. Product callers use `create_embedded_host()`,
 *       which selects the configured implementations once.
 */
std::unique_ptr<Host> create_embedded_host_with_dependencies(
    std::shared_ptr<const ImageArtifactCodec> image_codec,
    std::shared_ptr<const CacheMetadataCodec> metadata_codec,
    std::shared_ptr<const GraphDocumentReader> document_reader,
    std::shared_ptr<const GraphDocumentWriter> document_writer);

}  // namespace ps::internal
