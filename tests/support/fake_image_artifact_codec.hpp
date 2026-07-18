#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/image_artifact_codec.hpp"

namespace ps::testing {

/**
 * @class FakeImageArtifactCodec
 * @brief Deterministic in-memory codec for cache lifecycle and error tests.
 *
 * The fake records ordered decode/encode calls under one mutex and delegates
 * behavior to optional caller-supplied callbacks. It performs no filesystem
 * image decoding and owns no Graph or cache state.
 *
 * @note The fake is source-tree test support only. Shared ownership lets tests
 * retain an observer while Kernel or GraphCacheService retains the codec.
 */
class FakeImageArtifactCodec final : public ImageArtifactCodec {
 public:
  /** @brief One recorded codec invocation. */
  struct Call {
    /** @brief Kind of codec operation that was invoked. */
    enum class Kind {
      /** @brief Decode invocation. */
      Decode,
      /** @brief Encode invocation. */
      Encode,
    };

    /** @brief Operation kind. */
    Kind kind = Kind::Decode;
    /** @brief Path supplied by the cache owner. */
    std::filesystem::path path;
    /** @brief Encode precision; absent for decode calls. */
    std::optional<ImageArtifactPrecision> precision;
  };

  /**
   * @brief Callback used to synthesize one decode result or exception.
   * @param path Artifact path supplied by the caller.
   * @return Owned decoded image buffer.
   * @throws Any exception selected by the test.
   * @note The callback runs without the fake's mutex held.
   */
  using DecodeCallback =
      std::function<ImageBuffer(const std::filesystem::path& path)>;

  /**
   * @brief Callback used to observe or fail one encode request.
   * @param path Artifact path supplied by the caller.
   * @param image Borrowed image descriptor valid for the callback duration.
   * @param precision Requested normalized integer precision.
   * @return Nothing.
   * @throws Any exception selected by the test.
   * @note The callback runs without the fake's mutex held.
   */
  using EncodeCallback = std::function<void(const std::filesystem::path& path,
                                            const ImageBuffer& image,
                                            ImageArtifactPrecision precision)>;

  /**
   * @brief Creates a fake with optional decode and encode behavior.
   * @param decode Callback for decode calls; empty callbacks reject decoding.
   * @param encode Callback for encode calls; empty callbacks accept encoding.
   * @throws std::bad_alloc if callback ownership allocation fails.
   * @note Callback replacement after construction is intentionally unsupported,
   * keeping concurrent call behavior immutable.
   */
  explicit FakeImageArtifactCodec(DecodeCallback decode = {},
                                  EncodeCallback encode = {})
      : decode_(std::move(decode)), encode_(std::move(encode)) {}

  /**
   * @brief Records and delegates one decode request.
   * @param path Artifact path supplied by GraphCacheService.
   * @return Callback-produced image buffer.
   * @throws std::logic_error when no decode callback was configured.
   * @throws Any exception propagated by the configured callback.
   * @note Call recording completes before callback execution, including failure
   * paths.
   */
  ImageBuffer decode(const std::filesystem::path& path) const override {
    record(Call{Call::Kind::Decode, path, std::nullopt});
    if (!decode_) {
      throw std::logic_error("FakeImageArtifactCodec decode is not configured");
    }
    return decode_(path);
  }

  /**
   * @brief Records and delegates one encode request.
   * @param path Artifact path supplied by GraphCacheService.
   * @param image Borrowed image descriptor.
   * @param precision Requested integer storage precision.
   * @return Nothing.
   * @throws Any exception propagated by the configured callback.
   * @note With no encode callback the request is recorded and accepted without
   * writing bytes.
   */
  void encode(const std::filesystem::path& path, const ImageBuffer& image,
              ImageArtifactPrecision precision) const override {
    record(Call{Call::Kind::Encode, path, precision});
    if (encode_) {
      encode_(path, image, precision);
    }
  }

  /**
   * @brief Copies the ordered call history.
   * @return Value snapshot of every completed call-recording step.
   * @throws std::bad_alloc if snapshot allocation fails.
   * @note The returned vector is independent of future fake activity.
   */
  std::vector<Call> calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }

 private:
  /**
   * @brief Appends one call under the history mutex.
   * @param call Invocation record to retain.
   * @return Nothing.
   * @throws std::bad_alloc if history growth fails.
   * @note No callback is invoked while the mutex is held.
   */
  void record(Call call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    calls_.push_back(std::move(call));
  }

  /** @brief Immutable decode behavior owned by the fake. */
  DecodeCallback decode_;
  /** @brief Immutable encode behavior owned by the fake. */
  EncodeCallback encode_;
  /** @brief Serializes access to the mutable call history. */
  mutable std::mutex mutex_;
  /** @brief Ordered value records retained for test assertions. */
  mutable std::vector<Call> calls_;
};

}  // namespace ps::testing
