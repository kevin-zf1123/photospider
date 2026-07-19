#include "providers/configured_image_artifact_codec.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "photospider/core/graph_error.hpp"

#if defined(PHOTOSPIDER_HAS_OPENCV)
#include "adapters/opencv/image_artifact_codec_opencv.hpp"
#endif

namespace ps::providers {
namespace {

#if !defined(PHOTOSPIDER_HAS_OPENCV)

/**
 * @brief Reports unavailable image-artifact persistence without provider types.
 *
 * @note The codec owns no cache path, image, retry, or transaction state. It
 *       preserves non-null product composition while every representation-
 *       dependent operation fails explicitly with `GraphErrc::Io`.
 */
class UnavailableImageArtifactCodec final : public ImageArtifactCodec {
 public:
  /** @copydoc ImageArtifactCodec::decode */
  ImageBuffer decode(const std::filesystem::path& path) const override {
    throw GraphError(
        GraphErrc::Io,
        "Image artifact codec is disabled for this build: " + path.string());
  }

  /** @copydoc ImageArtifactCodec::encode */
  void encode(const std::filesystem::path& path, const ImageBuffer& image,
              ImageArtifactPrecision precision) const override {
    (void)image;
    (void)precision;
    throw GraphError(
        GraphErrc::Io,
        "Image artifact codec is disabled for this build: " + path.string());
  }
};

#endif

}  // namespace

/** @copydoc make_configured_image_artifact_codec */
std::shared_ptr<const ImageArtifactCodec>
make_configured_image_artifact_codec() {
  static std::once_flag once;
  static std::shared_ptr<const ImageArtifactCodec> codec;
  std::call_once(once, [] {
#if defined(PHOTOSPIDER_HAS_OPENCV)
    codec = std::make_shared<adapters::opencv::OpenCvImageArtifactCodec>();
#else
    codec = std::make_shared<UnavailableImageArtifactCodec>();
#endif
  });
  return codec;
}

}  // namespace ps::providers
