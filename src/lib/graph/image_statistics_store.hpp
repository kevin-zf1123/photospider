#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>

#include "photospider/data/image_statistics.hpp"

namespace ps {

/** @brief Default retained result bound for one graph-cache statistics store.
 */
inline constexpr std::size_t kDefaultImageStatisticsCacheEntries = 128U;

/** @brief Hard source-private bound for one statistics store configuration. */
inline constexpr std::size_t kMaximumImageStatisticsCacheEntries = 4096U;

/**
 * @brief Typed terminal failure for explicitly cancelled statistics work.
 * @throws std::bad_alloc when the inherited diagnostic cannot allocate.
 * @note Cancellation changes no Value, Region, graph, HP/RT generation, or
 * cached result. The exception is transported only by the scheduled future.
 */
class ImageStatisticsCancelled final : public std::runtime_error {
 public:
  /**
   * @brief Creates the stable cancellation diagnostic.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  ImageStatisticsCancelled();
};

/** @brief Opaque request-local cancellation/publication arbitration state. */
struct ImageStatisticsCancellationState;

/** @brief Opaque store state retained independently by accepted tasks. */
struct ImageStatisticsStoreState;

/**
 * @brief Move-only handle for one explicitly scheduled statistics request.
 *
 * The handle owns the sole future and shares only cancellation arbitration with
 * the scheduled task. Destroying the handle does not implicitly cancel accepted
 * work; callers requiring cancellation must invoke `cancel()` explicitly.
 *
 * @throws Nothing for move and destruction.
 * @note The handle owns no Value, graph, cache entry, scheduler, or worker. The
 * scheduled callback independently retains the exact source Value until it
 * settles.
 */
class ScheduledImageStatistics final {
 public:
  /**
   * @brief Transfers a scheduled handle without duplicating its future.
   * @param other Handle whose completion and cancellation state are moved.
   * @throws Nothing.
   * @note The moved-from handle may only be destroyed or assigned another
   * valid handle before its request methods are used again.
   */
  ScheduledImageStatistics(ScheduledImageStatistics&& other) noexcept = default;

  /**
   * @brief Replaces this handle through complete ownership transfer.
   * @param other Handle whose state replaces this one.
   * @return This handle after replacement.
   * @throws Nothing.
   */
  ScheduledImageStatistics& operator=(
      ScheduledImageStatistics&& other) noexcept = default;

  /** @brief Prevents duplicating the sole completion future. */
  ScheduledImageStatistics(const ScheduledImageStatistics&) = delete;

  /** @brief Prevents copy assignment of the sole completion future. */
  ScheduledImageStatistics& operator=(const ScheduledImageStatistics&) = delete;

  /**
   * @brief Requests cancellation at the request/publication arbitration point.
   * @return Nothing.
   * @throws std::system_error when request-state synchronization fails.
   * @note If publication already linearized, cancellation does not remove the
   * derived entry. If cancellation linearizes first, publication is forbidden.
   * The method requires a non-moved-from handle.
   */
  void cancel();

  /**
   * @brief Reports whether cancellation has linearized for this request.
   * @return True after `cancel()` wins the request-state mutex.
   * @throws std::system_error when request-state synchronization fails.
   * @note The method requires a non-moved-from handle.
   */
  bool cancelled() const;

  /**
   * @brief Transfers the sole future to the caller.
   * @return Future that yields a validated result or rethrows scan, scheduling,
   * cancellation, or allocation failure.
   * @throws std::future_error when the future was already transferred.
   * @note Taking the future does not cancel or detach the scheduled callback.
   * A moved-from handle also has no future state.
   */
  std::future<ImageStatisticsResult> take_completion();

 private:
  friend class ImageStatisticsStore;

  /**
   * @brief Creates a complete scheduled handle for the store.
   * @param completion Sole future already bound to the task promise.
   * @param cancellation Shared request-local arbitration state.
   * @throws Nothing.
   */
  ScheduledImageStatistics(
      std::future<ImageStatisticsResult> completion,
      std::shared_ptr<ImageStatisticsCancellationState> cancellation) noexcept;

  /** @brief Sole movable completion transport. */
  std::future<ImageStatisticsResult> completion_;

  /** @brief Shared request-local cancellation/publication arbitration. */
  std::shared_ptr<ImageStatisticsCancellationState> cancellation_;
};

/**
 * @brief Bounded thread-safe cache and scheduled producer for image statistics.
 *
 * Every scheduled task retains an immutable Ready image Value, constructs its
 * complete key from that Value revision plus the caller's optional content
 * digest and canonical query, scans only through `ImageView`, validates the
 * result, and attempts one cache publication. Entries are compared by complete
 * key and evicted in deterministic oldest-publication order.
 *
 * @throws std::invalid_argument for invalid bounds, keys, image metadata,
 * unsupported scan facts, or malformed selectors/Regions.
 * @throws std::bad_alloc when request, view, result, promise, task, or cache
 * storage cannot allocate.
 * @note The store owns no Value payload, Graph, HP/RT generation, scheduler,
 * worker, allocation identity, compatibility ImageBuffer, or persistent cache.
 * Result lookup, eviction, invalidation, failure, and cancellation never mutate
 * the referenced Value or any formal cache authority.
 */
class ImageStatisticsStore final {
 public:
  /** @brief Owned callback representing one scheduled statistics task. */
  using Task = std::function<void()>;

  /**
   * @brief Trusted internal task-ownership transfer selected by the caller.
   * @note The receiver must either take the task exactly once or throw before
   * invoking it; accepted tasks may run inline or asynchronously.
   */
  using Scheduler = std::function<void(Task)>;

  /**
   * @brief Creates an empty bounded derived-result store.
   * @param maximum_entries Positive retained entry limit no greater than the
   * frozen hard bound.
   * @throws std::invalid_argument when the limit is zero or excessive.
   * @note Construction creates no worker, Value, result, or scheduled task.
   */
  explicit ImageStatisticsStore(
      std::size_t maximum_entries = kDefaultImageStatisticsCacheEntries);

  /** @brief Prevents duplicating one cache-owner facade. */
  ImageStatisticsStore(const ImageStatisticsStore&) = delete;

  /** @brief Prevents assigning another cache-owner facade. */
  ImageStatisticsStore& operator=(const ImageStatisticsStore&) = delete;

  /** @brief Prevents moving a facade while callers may borrow its address. */
  ImageStatisticsStore(ImageStatisticsStore&&) = delete;

  /** @brief Prevents move assignment of a cache-owner facade. */
  ImageStatisticsStore& operator=(ImageStatisticsStore&&) = delete;

  /**
   * @brief Schedules or reuses one Value-backed statistics request.
   *
   * Cache lookup occurs before task construction. On a miss, the complete Value
   * and request state move into one callback transferred to `scheduler`.
   *
   * @param value Valid sealed image Value retained by scheduled work.
   * @param content_digest Optional already-known canonical logical content id.
   * @param query Complete canonical Region/selection/algorithm request.
   * @param scheduler Nonempty trusted one-task ownership receiver.
   * @return Move-only request with explicit cancellation and sole future.
   * @throws std::invalid_argument for invalid Value/query/digest or an empty
   * scheduler.
   * @throws std::length_error for an over-limit query.
   * @throws Scheduler, future, allocation, or synchronization exceptions from
   * request construction and task transfer.
   * @note A cache hit creates a ready future without invoking the scheduler.
   * Scan failures and cancellation settle the future and insert no entry.
   */
  ScheduledImageStatistics schedule(Value value,
                                    std::optional<ContentDigest> content_digest,
                                    ImageStatisticsQuery query,
                                    const Scheduler& scheduler) const;

  /**
   * @brief Looks up one complete derived cache identity.
   * @param key Complete statistics cache key.
   * @return Copied validated result, or nullopt when absent.
   * @throws Validation, allocation, or mutex exceptions.
   * @note Lookup does not retain or inspect a Value and changes no eviction
   * order.
   */
  std::optional<ImageStatisticsResult> lookup(
      const ImageStatisticsCacheKey& key) const;

  /**
   * @brief Removes every derived result for one exact Value revision.
   * @param revision Valid process-local Value revision.
   * @return Number of removed derived entries.
   * @throws std::invalid_argument when revision is invalid.
   * @note The referenced Value and all graph/formal cache facts are untouched.
   */
  std::size_t invalidate_revision(ValueRevisionId revision) const;

  /**
   * @brief Evicts every derived result in this store.
   * @return Number of removed entries.
   * @throws Mutex synchronization exceptions only.
   * @note In-flight work may later publish unless its request is cancelled; the
   * method owns no cancellation authority.
   */
  std::size_t clear() const;

  /**
   * @brief Returns the current retained result count.
   * @return Count not exceeding `maximum_entries()`.
   * @throws Mutex synchronization exceptions only.
   */
  std::size_t size() const;

  /**
   * @brief Returns the immutable retained-entry bound.
   * @return Positive configured bound.
   * @throws Nothing.
   */
  std::size_t maximum_entries() const noexcept;

 private:
  /**
   * @brief Publishes one validated result if cancellation has not won.
   * @param result Complete validated scan result.
   * @param cancellation Matching request arbitration state.
   * @return Existing result for the same key or the newly published result.
   * @throws ImageStatisticsCancelled when cancellation linearized first.
   * @throws std::bad_alloc when return/cache storage cannot allocate.
   * @note Cancellation and publication are serialized before the cache mutex;
   * a new entry is inserted once, then oldest entries are evicted to the bound.
   */
  static ImageStatisticsResult publish(
      ImageStatisticsResult result,
      const std::shared_ptr<ImageStatisticsStoreState>& state,
      const std::shared_ptr<ImageStatisticsCancellationState>& cancellation);

  /**
   * @brief Shared cache state retained by accepted tasks through settlement.
   * @note The facade remains the only API owner; task retention prevents a
   * use-after-free but grants no GraphCacheService or scheduler lifetime.
   */
  std::shared_ptr<ImageStatisticsStoreState> state_;
};

}  // namespace ps
