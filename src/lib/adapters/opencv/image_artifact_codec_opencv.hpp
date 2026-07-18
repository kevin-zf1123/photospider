#pragma once

#include "core/image_artifact_codec.hpp"

namespace ps::adapters::opencv {

/**
 * @class OpenCvImageArtifactCodec
 * @brief Implements image artifact persistence through OpenCV imgcodecs.
 *
 * The adapter owns OpenCV decode, scalar conversion, encode, and exception
 * translation. It exposes only kernel-owned paths, `ImageBuffer`, precision,
 * `GraphError`, and `std::bad_alloc` across the codec boundary.
 *
 * @note Independent calls share no mutable adapter state. OpenCV process policy
 * remains owned by the operation provider; this codec does not reconfigure it.
 */
class OpenCvImageArtifactCodec final : public ImageArtifactCodec {
 public:
  /**
   * @brief Decodes one image file and normalizes supported integer storage.
   * @param path Existing image artifact path.
   * @return Owned CPU float image buffer.
   * @throws std::bad_alloc when OpenCV reports `StsNoMem` or host ownership
   * allocation fails.
   * @throws GraphError with `GraphErrc::Io` for other OpenCV decode/conversion
   * failures or an empty decoded image.
   * @note Returned storage remains valid after provider-local `cv::Mat` handles
   * are destroyed.
   */
  ImageBuffer decode(const std::filesystem::path& path) const override;

  /**
   * @brief Encodes one CPU image through OpenCV imgcodecs.
   * @param path Destination image artifact path.
   * @param image Borrowed CPU image descriptor.
   * @param precision Unsigned normalized integer storage precision.
   * @return Nothing.
   * @throws std::bad_alloc when OpenCV reports `StsNoMem` or host allocation
   * fails.
   * @throws GraphError with `GraphErrc::Io` for invalid descriptors, conversion
   * failures, unsupported destinations, or failed writes.
   * @note The adapter borrows image storage only for this synchronous call.
   */
  void encode(const std::filesystem::path& path, const ImageBuffer& image,
              ImageArtifactPrecision precision) const override;
};

}  // namespace ps::adapters::opencv
