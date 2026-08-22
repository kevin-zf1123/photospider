#pragma once

#include "core/image_artifact_codec.hpp"

namespace ps::adapters::opencv {

/**
 * @brief Ordinary DenseImage file codec implemented through OpenCV imgcodecs.
 * @throws As documented by `ImageArtifactCodec`.
 * @note The adapter preserves decoded 8/16-bit code-value storage and applies
 *       conversion only through an explicit `SampleConversion` request.
 *       Encode validates a closed extension/depth/channel support matrix
 *       before `cv::imwrite`; it never accepts OpenCV's implicit CV_8U
 *       fallback. OpenEXR paths are rejected because only the dedicated
 *       OpenEXR codec can preserve independent signed data and display windows.
 */
class OpenCvImageArtifactCodec final : public ImageArtifactCodec {
 public:
  /** @copydoc ImageArtifactCodec::decode */
  Value decode(const std::filesystem::path& path,
               const ImageArtifactDecodeRequest& request) const override;

  /** @copydoc ImageArtifactCodec::encode */
  void encode(const std::filesystem::path& path, const Value& image,
              const ImageArtifactEncodeRequest& request) const override;
};

}  // namespace ps::adapters::opencv
