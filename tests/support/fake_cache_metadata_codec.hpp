#pragma once

#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/cache_metadata_codec.hpp"

namespace ps::testing {

/**
 * @class FakeCacheMetadataCodec
 * @brief Deterministic metadata codec for cache boundary and lifetime tests.
 *
 * The fake records ordered read/write calls under one mutex and delegates
 * behavior to immutable caller-supplied callbacks. It performs no parser or
 * filesystem metadata IO and owns no Graph or cache state.
 *
 * @note The fake is source-tree test support only. Shared ownership lets tests
 * retain an observer while Kernel or GraphCacheService retains the codec.
 */
class FakeCacheMetadataCodec final : public CacheMetadataCodec {
 public:
  /** @brief One recorded metadata-codec invocation. */
  struct Call {
    /** @brief Kind of metadata operation that was invoked. */
    enum class Kind {
      /** @brief Read invocation. */
      Read,
      /** @brief Write invocation. */
      Write,
    };

    /** @brief Operation kind. */
    Kind kind = Kind::Read;
    /** @brief Path supplied by the cache owner. */
    std::filesystem::path path;
    /** @brief Detached write values; empty for read calls. */
    plugin::ParameterMap values;
  };

  /**
   * @brief Callback used to synthesize one metadata read or exception.
   * @param path Metadata path supplied by the caller.
   * @return Detached named values.
   * @throws Any exception selected by the test.
   * @note The callback runs without the fake's mutex held.
   */
  using ReadCallback =
      std::function<plugin::ParameterMap(const std::filesystem::path& path)>;

  /**
   * @brief Callback used to observe or fail one metadata write.
   * @param path Metadata path supplied by the caller.
   * @param values Borrowed named values valid for the callback duration.
   * @return Nothing.
   * @throws Any exception selected by the test.
   * @note The callback runs without the fake's mutex held.
   */
  using WriteCallback = std::function<void(const std::filesystem::path& path,
                                           const plugin::ParameterMap& values)>;

  /**
   * @brief Creates a fake with optional read and write behavior.
   * @param read Callback for reads; an empty callback rejects reads.
   * @param write Callback for writes; an empty callback accepts writes.
   * @throws std::bad_alloc if callback ownership allocation fails.
   * @note Callback replacement is intentionally unsupported so independent
   * graph-state lanes observe immutable behavior.
   */
  explicit FakeCacheMetadataCodec(ReadCallback read = {},
                                  WriteCallback write = {})
      : read_(std::move(read)), write_(std::move(write)) {}

  /**
   * @brief Records and delegates one metadata read.
   * @param path Metadata path supplied by GraphCacheService.
   * @return Callback-produced detached named values.
   * @throws std::logic_error when no read callback was configured.
   * @throws Any exception propagated by the configured callback.
   * @note Call recording completes before callback execution, including failure
   * paths.
   */
  plugin::ParameterMap read(const std::filesystem::path& path) const override {
    record(Call{Call::Kind::Read, path, {}});
    if (!read_) {
      throw std::logic_error("FakeCacheMetadataCodec read is not configured");
    }
    return read_(path);
  }

  /**
   * @brief Records and delegates one metadata write.
   * @param path Metadata path supplied by GraphCacheService.
   * @param values Borrowed named values.
   * @return Nothing.
   * @throws Any exception propagated by the configured callback.
   * @note With no write callback the request is recorded and accepted without
   * writing bytes.
   */
  void write(const std::filesystem::path& path,
             const plugin::ParameterMap& values) const override {
    record(Call{Call::Kind::Write, path, values});
    if (write_) {
      write_(path, values);
    }
  }

  /**
   * @brief Copies the ordered call history.
   * @return Value snapshot of every completed call-recording step.
   * @throws std::bad_alloc if snapshot allocation fails.
   * @note The returned vector and parameter maps are detached from future fake
   * activity.
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
   * @throws std::bad_alloc if history growth or value copy fails.
   * @note No callback is invoked while the mutex is held.
   */
  void record(Call call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    calls_.push_back(std::move(call));
  }

  /** @brief Immutable read behavior owned by the fake. */
  ReadCallback read_;
  /** @brief Immutable write behavior owned by the fake. */
  WriteCallback write_;
  /** @brief Serializes access to the mutable call history. */
  mutable std::mutex mutex_;
  /** @brief Ordered detached call records retained for assertions. */
  mutable std::vector<Call> calls_;
};

}  // namespace ps::testing
