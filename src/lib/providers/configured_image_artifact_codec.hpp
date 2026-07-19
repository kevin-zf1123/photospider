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
 * @note OpenCV-enabled products return one immutable private OpenCV adapter.
 * OpenCV-disabled products return a dependency-neutral unavailable codec whose
 * operations report `GraphErrc::Io`. Tests may inject fakes through the
 * explicit private Host/Kernel seams without changing Graph/cache code.
 */
std::shared_ptr<const ImageArtifactCodec>
make_configured_image_artifact_codec();

}  // namespace ps::providers
