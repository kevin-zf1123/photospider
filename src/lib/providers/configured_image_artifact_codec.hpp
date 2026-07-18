#pragma once

#include <memory>

#include "core/image_artifact_codec.hpp"

namespace ps::providers {

/**
 * @brief Constructs the image artifact codec selected by the product build.
 * @return Process-shared codec owner injected into Kernel, GraphCacheService,
 * and configured source operations.
 * @throws std::bad_alloc if first-use codec ownership allocation fails.
 * @throws std::system_error if one-time initialization synchronization fails.
 * @note With the current product configuration this returns one immutable
 * private OpenCV adapter. Tests and future dependency-disabled composition
 * roots may inject a fake or another implementation without changing
 * Graph/cache code.
 */
std::shared_ptr<const ImageArtifactCodec>
make_configured_image_artifact_codec();

}  // namespace ps::providers
