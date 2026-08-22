#pragma once

#include "core/image_artifact_codec.hpp"

/**
 * @file openexr_dense_image_codec.hpp
 * @brief Source-private ordinary OpenEXR DenseImage Value codec.
 */

namespace ps::openexr_dense {

/**
 * @brief Encodes and decodes single-part scanline OpenEXR ordinary images.
 * @throws As documented by `ImageArtifactCodec`.
 * @note The codec preserves independent signed data and display windows and
 *       never handles deep, tiled, multipart, or variable-sample files. UINT
 *       and FLOAT channels retain their native 32-bit storage; HALF samples
 *       are promoted exactly to FP32 because the built-in DenseTensor contract
 *       has no binary16 storage encoding. Numeric conversion remains explicit.
 */
class OpenExrDenseImageCodec final : public ImageArtifactCodec {
 public:
  /** @copydoc ImageArtifactCodec::decode */
  Value decode(const std::filesystem::path& path,
               const ImageArtifactDecodeRequest& request) const override;

  /** @copydoc ImageArtifactCodec::encode */
  void encode(const std::filesystem::path& path, const Value& image,
              const ImageArtifactEncodeRequest& request) const override;
};

}  // namespace ps::openexr_dense
