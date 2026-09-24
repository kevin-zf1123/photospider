#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/core/status.hpp"

namespace ps {
class ResourceBudget;
namespace execution_internal {
struct CancellationPoll;
}  // namespace execution_internal
/** @brief Read-only cooperative cancellation. Copies share monotonic flags.
 * Observation never allocates or recursively traverses groups. Tokens from
 * managed sources retain their root capacity through the final token owner.
 */
class PHOTOSPIDER_API CancellationToken final {
 public:
  /** @brief Inert token, never cancelled; does not allocate. */
  CancellationToken() noexcept = default;
  [[nodiscard]] bool cancelled() const noexcept;
  /** @brief Flattens at most 64 distinct flags, without cancelling sources.
   * More than 64 arguments/flags returns ResourceExhausted. Inert arguments
   * contribute no flag. The ordinary overload may throw std::bad_alloc.
   */
  static Result<CancellationToken> combine(
      const std::vector<CancellationToken>& tokens);
  static Result<CancellationToken> combine(
      std::initializer_list<CancellationToken> tokens);
  /** @brief Same grouping with metadata admitted before allocation and owned
   * until the last group/source token retires. Exhaustion returns a status.
   */
  static Result<CancellationToken> combine(
      const std::vector<CancellationToken>& tokens,
      const ResourceBudget& budget);
  static Result<CancellationToken> combine(
      std::initializer_list<CancellationToken> tokens,
      const ResourceBudget& budget);

 private:
  friend class CancellationSource;
  friend struct execution_internal::CancellationPoll;
  struct State;
  static Result<CancellationToken> combine_impl(const CancellationToken* tokens,
                                                std::size_t count,
                                                const ResourceBudget* budget);
  explicit CancellationToken(std::shared_ptr<const State> state) noexcept
      : state_(std::move(state)) {}
  std::shared_ptr<const State> state_;
};
/** @brief Owns an idempotent, monotonic cooperative cancellation flag. */
class PHOTOSPIDER_API CancellationSource final {
 public:
  /** @brief Ordinary caller-owned flag; may throw std::bad_alloc. */
  CancellationSource();
  /** @brief Root-admitted flag; allocation failure throws std::bad_alloc. */
  explicit CancellationSource(const ResourceBudget& budget);
  [[nodiscard]] CancellationToken token() const noexcept {
    return CancellationToken(state_);
  }
  /** @brief True only on the first false-to-true transition; never allocates.
   */
  bool cancel() noexcept;

 private:
  std::shared_ptr<CancellationToken::State> state_;
};
}  // namespace ps
