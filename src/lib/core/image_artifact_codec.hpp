#pragma once

#include <filesystem>

#include "photospider/core/image_buffer.hpp"

namespace ps {

/**
 * @brief Normalized integer precision used for persisted image artifacts.
 *
 * The value describes how normalized floating-point image samples are encoded
 * on disk. It does not change the in-memory `ImageBuffer` scalar type.
 *
 * @note This private kernel contract is dependency-neutral and is not part of
 * the installed public ABI.
 */
enum class ImageArtifactPrecision {
  /** @brief Encode normalized samples with unsigned 8-bit storage. */
  UInt8,
  /** @brief Encode normalized samples with unsigned 16-bit storage. */
  UInt16,
};

/**
 * @class ImageArtifactCodec
 * @brief Dependency-neutral boundary for image artifact decode and encode.
 *
 * Implementations translate between filesystem image artifacts and owned
 * `ImageBuffer` values. Graph/cache code supplies paths, image descriptors, and
 * normalized integer precision without including or retaining provider-library
 * types.
 *
 * @note Implementations must be safe for independent calls from serialized
 * graph-state lanes belonging to different graphs. The interface owns no cache
 * policy, path identity, retry, or transaction semantics. Callers retain the
 * codec through shared ownership for the full service lifetime.
 */
class ImageArtifactCodec {
 public:
  /**
   * @brief Destroys one codec after every retaining service releases it.
   * @throws Nothing.
   * @note Destruction must not perform caller-visible cache publication.
   */
  virtual ~ImageArtifactCodec() = default;

  /**
   * @brief Decodes one image artifact into an owned float image buffer.
   *
   * @param path Existing artifact path selected by the cache owner.
   * @return Owned CPU `ImageBuffer`; unsigned 8/16-bit artifacts are normalized
   * to floating-point `[0,1]` samples.
   * @throws std::bad_alloc when host or provider allocation is exhausted.
   * @throws GraphError with `GraphErrc::Io` when the artifact cannot be opened,
   * decoded, converted, or represented by the implementation.
   * @note The returned payload must remain valid after this call and after any
   * provider-local temporary image handle is destroyed.
   */
  virtual ImageBuffer decode(const std::filesystem::path& path) const = 0;

  /**
   * @brief Encodes one CPU image buffer as a normalized integer artifact.
   *
   * @param path Destination artifact path prepared by the cache owner.
   * @param image Borrowed CPU image descriptor whose payload remains alive for
   * the call.
   * @param precision Integer storage precision selected by cache policy.
   * @return Nothing.
   * @throws std::bad_alloc when host or provider allocation is exhausted.
   * @throws GraphError with `GraphErrc::Io` when the descriptor cannot be
   * encoded or the destination write fails.
   * @note The method does not own directory creation, metadata serialization,
   * atomic replacement, retry, or cache visibility policy.
   */
  virtual void encode(const std::filesystem::path& path,
                      const ImageBuffer& image,
                      ImageArtifactPrecision precision) const = 0;
};

}  // namespace ps
