#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "photospider/core/export.hpp"

namespace ps {

/**
 * @brief Read-only cooperative cancellation observation.
 *
 * @note Tokens are cheap copies sharing one monotonic false-to-true flag.
 */
class PHOTOSPIDER_API CancellationToken final {
 public:
  /**
   * @brief Constructs an inert token that is never cancelled.
   * @throws Nothing.
   * @note Used when a caller does not need cancellation.
   */
  CancellationToken() noexcept = default;

  /**
   * @brief Reports whether cancellation was requested.
   * @return True after the owning source accepts cancellation.
   * @throws Nothing.
   * @note Observation is lock-free where the platform atomic permits it.
   */
  [[nodiscard]] bool cancelled() const noexcept {
    return state_ && state_->load(std::memory_order_acquire);
  }

 private:
  friend class CancellationSource;

  /**
   * @brief Constructs a token sharing one source flag.
   * @param state Shared monotonic cancellation flag.
   * @throws Nothing.
   * @note Only CancellationSource can create an active token.
   */
  explicit CancellationToken(std::shared_ptr<std::atomic<bool>> state) noexcept
      : state_(std::move(state)) {}

  /** @brief Shared monotonic flag; null means cancellation is unsupported. */
  std::shared_ptr<std::atomic<bool>> state_;
};

/**
 * @brief Owns one cooperative cancellation flag.
 *
 * @note Requesting cancellation is idempotent and cannot be reset.
 */
class PHOTOSPIDER_API CancellationSource final {
 public:
  /**
   * @brief Creates one uncancelled source.
   * @throws std::bad_alloc If the shared flag cannot be allocated.
   * @note The source and all tokens share the flag lifetime.
   */
  CancellationSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}

  /**
   * @brief Creates a read-only token.
   * @return Token sharing this source's state.
   * @throws Nothing.
   * @note Token destruction does not request cancellation.
   */
  [[nodiscard]] CancellationToken token() const noexcept {
    return CancellationToken(state_);
  }

  /**
   * @brief Requests cooperative cancellation.
   * @return True only for the first false-to-true transition.
   * @throws Nothing.
   * @note Running native callbacks are not forcefully preempted.
   */
  bool cancel() noexcept {
    bool expected = false;
    return state_->compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel);
  }

 private:
  /** @brief Shared monotonic state retained by issued tokens. */
  std::shared_ptr<std::atomic<bool>> state_;
};

}  // namespace ps
