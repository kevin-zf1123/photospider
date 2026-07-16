#pragma once

#include <optional>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)

namespace ps::compute {

enum class CacheReadMode {
  ReusableOnly,
  InteractivePreferred,
};

/**
 * @brief Central policy for formal HP cache access on GraphModel nodes.
 *
 * ComputeCachePolicy intentionally treats Node as HP-only cache authority.
 * Real-time preview output lives in RealtimeProxyGraph and is resolved by RT
 * executors with an explicit proxy lookup instead of node-local cache fields.
 *
 * @note CacheReadMode::InteractivePreferred is retained for older resolver
 * call shapes but currently degrades to reusable HP output at the node layer.
 */
class ComputeCachePolicy {
 public:
  /**
   * @brief Checks whether a node has reusable HP output.
   *
   * @param node Node whose formal cache should be inspected.
   * @return True when cached_output_high_precision is populated.
   * @throws Nothing.
   */
  static bool has_reusable_output(const Node& node);

  /**
   * @brief Returns mutable reusable HP output when available.
   *
   * @param node Node whose HP cache should be inspected.
   * @return Pointer to HP output, or nullptr when absent.
   * @throws Nothing.
   */
  static NodeOutput* reusable_output(Node& node);

  /**
   * @brief Returns immutable reusable HP output when available.
   *
   * @param node Node whose HP cache should be inspected.
   * @return Pointer to HP output, or nullptr when absent.
   * @throws Nothing.
   */
  static const NodeOutput* reusable_output(const Node& node);

  /**
   * @brief Selects node-local output for a cache read mode.
   *
   * @param node Node whose output should be inspected.
   * @param mode Requested read mode; InteractivePreferred is accepted for
   * compatibility but returns HP output only.
   * @return Optional pointer to reusable HP output.
   * @throws Nothing.
   * @note RT proxy output is not node-local and is therefore never returned.
   */
  static std::optional<const NodeOutput*> select_output(const Node& node,
                                                        CacheReadMode mode);

  /**
   * @brief Reports whether disk cache reads are allowed for a request.
   *
   * @param disable_disk_cache Request flag disabling disk reads.
   * @param force_recache Request flag forcing recompute.
   * @return True when disk cache may be read.
   * @throws Nothing.
   */
  static bool can_read_disk_cache(bool disable_disk_cache, bool force_recache);

  /**
   * @brief Clears formal HP output before a recompute.
   *
   * @param node Node whose HP cache should be reset.
   * @throws Nothing directly.
   * @note RT proxy state is owned outside GraphModel and is not affected.
   */
  static void clear_for_recompute(Node& node);
};

}  // namespace ps::compute
