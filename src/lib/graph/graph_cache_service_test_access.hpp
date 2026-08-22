#pragma once

#include <filesystem>

namespace ps::testing {

/**
 * @brief Deterministic checkpoints from disk-cache transactions.
 * @throws Nothing for value construction and comparison.
 * @note This contract exists only in test-enabled product builds and is not
 * installed or exposed through Host.
 */
enum class GraphCacheServiceTestEvent {
  /** @brief The cache root was removed and has not yet been recreated. */
  DriveCacheRootRemoved,
  /** @brief A validated manifest was detached before payload acquisition. */
  ManifestReadBeforePayload,
  /** @brief Archive stat/type/sparse/limit checks passed before allocation. */
  ArchiveAllocationApproved,
  /** @brief An admitted async writer is about to acquire root coordination. */
  AsyncWriterBeforeRootLock,
  /**
   * @brief One cache operation reached its root mutex before lock acquisition.
   * @note Tests arm this checkpoint only after another operation has proved it
   *       owns the same root. The callback runs before `try_lock` and must not
   *       re-enter the observed root.
   */
  RootOperationBeforeLock,
  /**
   * @brief The root mutex was already owned when an operation tried it.
   * @note The operation blocks on the same mutex only after this positive
   *       contention checkpoint returns.
   */
  RootOperationLockContended,
  /**
   * @brief The operation acquired its root mutex without contention.
   * @note An armed overlapping-operation test treats this event as evidence
   *       that the two operations did not share one coordinator.
   */
  RootOperationLockAcquiredWithoutContention,
};

/**
 * @brief Borrowed observer for deterministic post-removal failure injection.
 * @throws Nothing for aggregate construction.
 * @note Tests retain the hook and context until the cache operation settles.
 * The callback may throw to emulate a failure immediately before cache-root
 * recreation and must not re-enter the same graph-state lane.
 */
struct GraphCacheServiceTestHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Observes one exact disk-cache checkpoint.
   * @param context Borrowed context supplied by the installing test.
   * @param event Exact checkpoint reached by the product cache service.
   * @param cache_root Cache scope associated with the exact checkpoint.
   * @return Nothing.
   * @throws Any exception selected by the deterministic fault injector.
   */
  void (*notify)(void* context, GraphCacheServiceTestEvent event,
                 const std::filesystem::path& cache_root) = nullptr;
};

/**
 * @brief Installs or clears the process-local cache-clear observer.
 * @param hook Borrowed hook that outlives in-flight notifications, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_graph_cache_service_test_hook(
    const GraphCacheServiceTestHook* hook) noexcept;

/**
 * @brief Publishes one cache-clear checkpoint to the installed observer.
 * @param event Exact checkpoint reached by the product cache service.
 * @param cache_root Cache scope associated with the exact checkpoint.
 * @return Nothing.
 * @throws Any exception selected by the installed observer.
 */
void notify_graph_cache_service_test_hook(
    GraphCacheServiceTestEvent event, const std::filesystem::path& cache_root);

}  // namespace ps::testing
