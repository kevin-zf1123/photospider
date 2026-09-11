#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/core/status.hpp"

namespace ps {

/**
 * @brief Read-only cooperative cancellation observation.
 *
 * @note Tokens are cheap copies observing immutable groups of monotonic flags.
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
    if (!state_)
      return false;
    if (state_->flag.load(std::memory_order_acquire))
      return true;
    for (const auto& source : state_->sources)
      if (source->flag.load(std::memory_order_acquire))
        return true;
    return false;
  }
  /** @brief Observes cancellation from any of at most 64 distinct sources.
   * @note Groups are flattened and deduplicated; observation never recurses or
   * allocates and cancelling one source does not cancel any other source.
   * @return Inert/group token or ResourceExhausted for more than 64 arguments
   * or distinct flags. Inert arguments do not create a flag.
   * @throws std::bad_alloc For bounded immutable group metadata.
   */
  static Result<CancellationToken> combine(
      const std::vector<CancellationToken>& tokens) {
    if (tokens.size() > 64)
      return Result<CancellationToken>(
          Status{ErrorCode::ResourceExhausted, {}});
    auto state = std::make_shared<State>();
    auto add = [&](const std::shared_ptr<const State>& source) {
      if (std::find(state->sources.begin(), state->sources.end(), source) !=
          state->sources.end())
        return true;
      if (state->sources.size() == 64)
        return false;
      state->sources.push_back(source);
      return true;
    };
    for (const auto& token : tokens) {
      if (!token.state_)
        continue;
      if (token.state_->sources.empty()) {
        if (!add(token.state_))
          return Result<CancellationToken>(
              Status{ErrorCode::ResourceExhausted, {}});
      } else {
        for (const auto& source : token.state_->sources)
          if (!add(source))
            return Result<CancellationToken>(
                Status{ErrorCode::ResourceExhausted, {}});
      }
    }
    if (state->sources.empty())
      return Result<CancellationToken>(CancellationToken{});
    if (state->sources.size() == 1)
      return Result<CancellationToken>(CancellationToken(state->sources[0]));
    return Result<CancellationToken>(CancellationToken(std::move(state)));
  }

 private:
  friend class CancellationSource;
  struct State {
    std::atomic<bool> flag{false};
    std::vector<std::shared_ptr<const State>> sources;
  };

  /**
   * @brief Constructs a token sharing one source flag.
   * @param state Shared monotonic cancellation flag.
   * @throws Nothing.
   * @note Only CancellationSource can create an active token.
   */
  explicit CancellationToken(std::shared_ptr<const State> state) noexcept
      : state_(std::move(state)) {}

  /** @brief Shared monotonic flag; null means cancellation is unsupported. */
  std::shared_ptr<const State> state_;
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
  CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

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
    return state_->flag.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel);
  }

 private:
  /** @brief Shared monotonic state retained by issued tokens. */
  std::shared_ptr<CancellationToken::State> state_;
};

}  // namespace ps
