#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphCacheService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Serializes side effects after scheduler worker tasks finish.
 *
 * ComputeResultCommitter owns the post-dispatch mutation phase that workers
 * deliberately avoid: timing total calculation, GraphModel high-precision
 * cache updates, HP version increments, and configured disk cache writes.
 *
 * @note commit() holds the graph mutex while moving temp outputs into node
 * runtime state so GraphModel remains the sole owner of committed HP cache
 * entries.
 */
class ComputeResultCommitter {
 public:
  /**
   * @brief Binds the committer to cache and graph synchronization state.
   *
   * @param cache Cache service used for optional disk cache writes.
   * @param graph_mutex Graph mutex guarding runtime cache mutation.
   * @param cache_precision Cache precision label forwarded to cache writes.
   * @throws Nothing directly.
   * @note The committer stores references only and must not outlive the graph
   * and cache service used for the active dispatch.
   */
  ComputeResultCommitter(GraphCacheService& cache, std::mutex& graph_mutex,
                         const std::string& cache_precision);

  /**
   * @brief Recomputes total timed execution duration from node timings.
   *
   * @param timing_results Timing collector to finalize.
   * @param timing_mutex Mutex protecting timing_results.
   * @throws Nothing directly.
   * @note The total is recomputed after all workers finish to avoid concurrent
   * aggregate mutation during task execution.
   */
  void finalize_timing(TimingCollector& timing_results,
                       std::mutex& timing_mutex) const;

  /**
   * @brief Commits populated temp outputs into GraphModel runtime cache state.
   *
   * @param graph Graph whose high-precision node caches are updated.
   * @param execution_order Dense planned node id order.
   * @param temp_results Temporary outputs aligned with execution_order.
   * @throws Exceptions from GraphModel mutation or GraphCacheService writes.
   * @note temp_results values are moved. After commit(), populated slots no
   * longer own valid output values.
   */
  void commit(GraphModel& graph, const std::vector<int>& execution_order,
              std::vector<std::optional<NodeOutput>>& temp_results) const;

 private:
  /** @brief Borrowed cache service used after GraphModel mutation. */
  GraphCacheService& cache_;

  /** @brief Borrowed graph mutex held during committed cache mutation. */
  std::mutex& graph_mutex_;

  /** @brief Cache precision label used by save_cache_if_configured(). */
  const std::string& cache_precision_;
};

/**
 * @brief Clears high-precision memory cache for nodes in a planned dispatch.
 *
 * @param graph Graph whose planned node HP caches are cleared.
 * @param graph_mutex Mutex guarding node runtime state mutation.
 * @param order Planned node ids whose HP cache entries should be reset.
 * @throws Exceptions from GraphModel runtime-state mutation.
 * @note Missing node ids are skipped so stale diagnostic plan ids do not cause
 * recache requests to fail before execution starts.
 */
void clear_planned_high_precision_caches(GraphModel& graph,
                                         std::mutex& graph_mutex,
                                         const std::vector<int>& order);

}  // namespace ps::compute
