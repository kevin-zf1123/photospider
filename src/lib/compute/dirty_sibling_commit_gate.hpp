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
 * GraphModel. If RT fails, HP staged output is abandoned before graph commit.
 *
 * @note The gate owns no graph or buffer state. It is request-local and shared
 * only by callbacks launched for the same RealTimeUpdate request.
 */
class DirtySiblingCommitGate {
 public:
  /**
   * @brief Blocks until RT proxy commit succeeds or HP commit is aborted.
   *
   * @throws GraphError when abort_hp_commit() was called before RT commit.
   * @note HP dirty executor calls this immediately before committing staged HP
   * output to GraphModel.
   */
  void wait_for_rt_commit_or_throw();

  /**
   * @brief Marks the RT proxy commit as complete and releases HP commit.
   *
   * @throws Nothing.
   * @note Calling this after abort is harmless; waiting HP commits will still
   * observe the abort state first only if abort happened before this call.
   */
  void mark_rt_committed();

  /**
   * @brief Prevents HP staged output from committing to GraphModel.
   *
   * @throws Nothing.
   * @note The coordinator calls this when RT sibling execution throws before
   * successfully committing proxy output.
   */
  void abort_hp_commit();

 private:
  /** @brief Mutex protecting gate state. */
  std::mutex mutex_;

  /** @brief Condition variable used by HP commit waiters. */
  std::condition_variable condition_;

  /** @brief True after RT proxy output has been committed. */
  bool rt_committed_ = false;

  /** @brief True when HP graph commit must be skipped. */
  bool aborted_ = false;
};

}  // namespace ps::compute
