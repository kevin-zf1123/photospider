#include "providers/configured_image_artifact_codec.hpp"

#include <memory>
#include <mutex>

#include "adapters/opencv/image_artifact_codec_opencv.hpp"

namespace ps::providers {

/** @copydoc make_configured_image_artifact_codec */
std::shared_ptr<const ImageArtifactCodec>
make_configured_image_artifact_codec() {
  static std::once_flag once;
  static std::shared_ptr<const ImageArtifactCodec> codec;
  std::call_once(once, [] {
    codec = std::make_shared<adapters::opencv::OpenCvImageArtifactCodec>();
  });
  return codec;
}

}  // namespace ps::providers
