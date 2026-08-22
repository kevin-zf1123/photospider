#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "photospider/data/sample_conversion.hpp"

namespace ps {

/**
 * @brief One exact storage-selected semantic rule for image-file decode.
 * @throws Nothing for ordinary value operations.
 * @note Storage selection is exact and caller-declared; the codec never maps a
 *       depth to sample meaning unless a matching rule exists.
 */
struct ImageArtifactDecodeRule final {
  /** @brief Logical scalar semantics required from decoded storage. */
  ElementSemantics element_semantics = ElementSemantics::UnsignedInteger;
  /** @brief Exact physical scalar encoding required from decoded storage. */
  StorageEncoding storage_encoding{8U};
  /** @brief Exact meaning assigned to matching decoded values. */
  SampleEndpoint encoded_samples;
  /**
   * @brief Optional explicit conversion applied after storage-preserving
   * decode.
   * @note Its source endpoint must equal `encoded_samples`.
   */
  std::optional<SampleConversion> conversion;
};

/**
 * @brief Explicit bounded semantic request for decoding one image file.
 * @throws std::bad_alloc when rule ownership allocates.
 * @note Rules must be strictly ordered by semantics, encoding kind, then bit
 *       width and must not duplicate a storage key.
 */
struct ImageArtifactDecodeRequest final {
  /** @brief Exact supported decoded storage rules. */
  std::vector<ImageArtifactDecodeRule> rules;
};

/**
 * @brief Explicit semantic request for encoding one ordinary image file.
 * @throws Nothing for ordinary value operations.
 * @note No conversion means the Value's declared storage and sample meaning
 *       must already be supported exactly by the selected codec.
 */
struct ImageArtifactEncodeRequest final {
  /** @brief Optional explicit source-to-file sample/storage conversion. */
  std::optional<SampleConversion> conversion;
};

/**
 * @brief Dependency-neutral ordinary DenseImage file codec boundary.
 * @throws As documented by individual operations.
 * @note The interface owns no cache, path-selection, sidecar, atomic replace,
 *       retry, or durable publication policy. Deep/variable-sample data uses
 *       its provider-defined adapter rather than this ordinary-image path.
 */
class ImageArtifactCodec {
 public:
  /** @brief Releases a codec after all synchronous calls finish. */
  virtual ~ImageArtifactCodec() = default;

  /**
   * @brief Decodes one image file without implicit sample normalization.
   * @param path Existing file selected by the caller.
   * @param request Explicit decoded sample meaning and optional conversion.
   * @return Fresh Ready ordinary DenseImage Value.
   * @throws std::invalid_argument for malformed semantic policy or unsupported
   *         file/storage facts.
   * @throws std::domain_error for rejected sample conversion.
   * @throws std::bad_alloc when codec or Value allocation fails.
   * @throws GraphError with `GraphErrc::Io` when file decode fails.
   * @note A codec preserves signed data/display-window facts when its format
   *       carries them. Formats without that authority assign only an explicit
   *       zero-origin data window and leave the display window absent.
   */
  virtual Value decode(const std::filesystem::path& path,
                       const ImageArtifactDecodeRequest& request) const = 0;

  /**
   * @brief Encodes one ordinary image with explicit sample policy.
   * @param path Destination selected by the caller.
   * @param image Ready Host-readable ordinary DenseImage Value.
   * @param request Explicit optional conversion to file storage/meaning.
   * @return Nothing after synchronous write succeeds.
   * @throws std::invalid_argument for missing/inconsistent sample facts,
   *         unsupported storage/layout, or malformed policy.
   * @throws std::domain_error for rejected out-of-domain, non-finite, range,
   *         rounding, or precision-loss cases.
   * @throws std::bad_alloc when conversion or codec allocation fails.
   * @throws GraphError with `GraphErrc::Io` when file encode fails.
   * @note No hidden scaling, clamp, rounding, non-finite replacement, or color
   *       transform is permitted.
   */
  virtual void encode(const std::filesystem::path& path, const Value& image,
                      const ImageArtifactEncodeRequest& request) const = 0;
};

}  // namespace ps
