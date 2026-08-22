#include "providers/configured_image_artifact_codec.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/core/graph_error.hpp"

#if defined(PHOTOSPIDER_HAS_OPENCV)
#include "adapters/opencv/image_artifact_codec_opencv.hpp"
#endif
#if defined(PHOTOSPIDER_HAS_OPENEXR_DENSE)
#include "adapters/openexr/openexr_dense_image_codec.hpp"
#endif

namespace ps::providers {
namespace {

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
  Value decode(const std::filesystem::path& path,
               const ImageArtifactDecodeRequest& request) const override {
    (void)request;
    throw GraphError(
        GraphErrc::Io,
        "Image artifact codec is disabled for this build: " + path.string());
  }

  /** @copydoc ImageArtifactCodec::encode */
  void encode(const std::filesystem::path& path, const Value& image,
              const ImageArtifactEncodeRequest& request) const override {
    (void)image;
    (void)request;
    throw GraphError(
        GraphErrc::Io,
        "Image artifact codec is disabled for this build: " + path.string());
  }
};

#if defined(PHOTOSPIDER_HAS_OPENEXR_DENSE)

/**
 * @brief Reports whether one artifact path selects the OpenEXR codec.
 * @param path Caller-selected path.
 * @return True for a case-insensitive `.exr` extension.
 * @throws std::bad_alloc when extension conversion allocates.
 */
bool is_openexr_path(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return extension == ".exr";
}

/**
 * @brief Routes OpenEXR paths to the signed-window codec and all other paths
 *        to the configured ordinary fallback.
 * @throws std::bad_alloc when retained codec ownership allocates.
 * @note Routing uses only the explicit filename extension. It performs no
 *       content sniffing, conversion, retry, cache, or persistence policy.
 */
class ExtensionRoutedImageArtifactCodec final : public ImageArtifactCodec {
 public:
  /**
   * @brief Retains both immutable codec implementations.
   * @param fallback Non-null codec for non-OpenEXR paths.
   * @param openexr Non-null dedicated ordinary OpenEXR codec.
   * @throws std::invalid_argument when either owner is null.
   */
  ExtensionRoutedImageArtifactCodec(
      std::shared_ptr<const ImageArtifactCodec> fallback,
      std::shared_ptr<const ImageArtifactCodec> openexr)
      : fallback_(std::move(fallback)), openexr_(std::move(openexr)) {
    if (fallback_ == nullptr || openexr_ == nullptr) {
      throw std::invalid_argument(
          "Configured image codec routing requires both implementations.");
    }
  }

  /** @copydoc ImageArtifactCodec::decode */
  Value decode(const std::filesystem::path& path,
               const ImageArtifactDecodeRequest& request) const override {
    return selected(path).decode(path, request);
  }

  /** @copydoc ImageArtifactCodec::encode */
  void encode(const std::filesystem::path& path, const Value& image,
              const ImageArtifactEncodeRequest& request) const override {
    selected(path).encode(path, image, request);
  }

 private:
  /**
   * @brief Selects one retained codec from the explicit path extension.
   * @param path Caller-selected artifact path.
   * @return Borrowed immutable codec.
   * @throws std::bad_alloc when extension conversion allocates.
   */
  const ImageArtifactCodec& selected(const std::filesystem::path& path) const {
    return is_openexr_path(path) ? *openexr_ : *fallback_;
  }

  /** @brief Configured non-OpenEXR codec or explicit unavailable sentinel. */
  std::shared_ptr<const ImageArtifactCodec> fallback_;
  /** @brief Dedicated independent-window ordinary OpenEXR codec. */
  std::shared_ptr<const ImageArtifactCodec> openexr_;
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
    std::shared_ptr<const ImageArtifactCodec> fallback =
        std::make_shared<adapters::opencv::OpenCvImageArtifactCodec>();
#else
    std::shared_ptr<const ImageArtifactCodec> fallback =
        std::make_shared<UnavailableImageArtifactCodec>();
#endif
#if defined(PHOTOSPIDER_HAS_OPENEXR_DENSE)
    codec = std::make_shared<ExtensionRoutedImageArtifactCodec>(
        std::move(fallback),
        std::make_shared<openexr_dense::OpenExrDenseImageCodec>());
#else
    codec = std::move(fallback);
#endif
  });
  return codec;
}

}  // namespace ps::providers
