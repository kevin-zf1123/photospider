#pragma once

#include <condition_variable>
#include <mutex>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Coordinates HP graph commit behind RT proxy commit for sibling work.
 *
 * DirtySiblingCommitGate lets HP and RT dirty siblings compute concurrently
 * while preserving the commit ordering required by RealTimeUpdate: RT proxy
 * output commits first, then HP staged output may mutate the original
 * GraphModel. If RT fails or is cancelled first, HP staged output is abandoned
 * before graph commit.
 *
 * @note The gate owns no graph or buffer state. It is request-local and shared
 * only by callbacks launched for the same RealTimeUpdate request.
 */
class DirtySiblingCommitGate {
 public:
  /**
   * @brief Blocks until RT proxy commit succeeds or HP commit is aborted.
   *
   * @return Nothing after RT commit permanently permits HP publication.
   * @throws GraphError when abort_hp_commit() was called before RT commit.
   * @throws std::system_error when mutex or condition-variable synchronization
   * fails.
   * @note HP dirty executor calls this immediately before committing staged HP
   * output to GraphModel.
   */
  void wait_for_rt_commit_or_throw();

  /**
   * @brief Marks the RT proxy commit as complete and releases HP commit.
   *
   * @return Nothing.
   * @throws std::system_error when synchronization fails.
   * @note The first pending decision is permanent. Calling this after denial is
   * a no-op and every current or future HP waiter continues to observe denial.
   */
  void mark_rt_committed();

  /**
   * @brief Prevents HP staged output from committing to GraphModel.
   *
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates because this noexcept
   * denial path cannot leave sibling commit authority ambiguous.
   * @note The coordinator calls this when RT sibling execution throws before
   * successfully committing proxy output. Calling it after RT committed is a
   * no-op and cannot revoke an already permitted HP commit.
   */
  void abort_hp_commit() noexcept;

 private:
  /**
   * @brief Monotonic RT-to-HP commit decision.
   * @throws Nothing for value operations.
   */
  enum class State {
    /** @brief Neither RT commit nor permanent denial has won. */
    Pending,
    /** @brief RT proxy publication completed before any denial. */
    Committed,
    /** @brief RT failure/cancellation permanently denied HP commit. */
    Denied,
  };

  /** @brief Mutex protecting gate state. */
  std::mutex mutex_;

  /** @brief Condition variable used by HP commit waiters. */
  std::condition_variable condition_;

  /** @brief Monotonic pending/committed/denied decision. */
  State state_ = State::Pending;
};

}  // namespace ps::compute
