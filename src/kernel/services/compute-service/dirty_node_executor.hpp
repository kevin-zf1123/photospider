#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/downsample_executor.hpp"
#include "kernel/services/compute-service/node_input_resolver.hpp"

namespace ps {
class GraphEventService;
class GraphModel;
class GraphRuntime;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Request-local mutexes keyed by graph node id for dirty task writes.
 *
 * Dirty tile tasks execute with local Node copies, but they still share cache
 * buffers and ROI/version metadata on the graph node. The map is built from a
 * dirty-pruned ComputePlan and borrowed by HP/RT dirty executors until all
 * scheduler callbacks for the request complete.
 */
using DirtyNodeMutexMap = std::unordered_map<int, std::shared_ptr<std::mutex>>;

/**
 * @brief Borrowed execution context shared by HP and RT dirty node executors.
 *
 * The context groups graph access, optional scheduler runtime, event recording,
 * dirty-source membership, and generation metadata for one dirty update. It is
 * an aggregate so call sites can construct it without introducing a long
 * constructor parameter list.
 *
 * @note All references are borrowed for scheduler callbacks created by the
 * owning dirty update executor. The context must not outlive the graph lock
 * and prepared dirty plan that produced it.
 */
struct DirtyNodeExecutionContext {
  /** @brief Graph used for dependency lookup, tiled execution, and commits. */
  GraphModel& graph;

  /** @brief Optional runtime for scheduler trace and stale-generation events.
   */
  GraphRuntime* runtime;

  /** @brief Event sink for HP/RT dirty node update events. */
  GraphEventService& events;

  /** @brief Dirty snapshot that marks source boundary nodes. */
  const DirtyRegionSnapshot& snapshot;

  /** @brief Dirty generation used to reject stale source callbacks. */
  uint64_t dirty_generation;

  /** @brief Per-node mutexes protecting shared dirty cache commits. */
  DirtyNodeMutexMap& node_mutexes;
};

/**
 * @brief Shared downsample queue sink populated by HP dirty node commits.
 *
 * HP dirty tasks may run through scheduler worker callbacks, so the vector is
 * protected by a mutex while node-level executors append refresh requests.
 *
 * @note The sink is borrowed from HighPrecisionDirtyExecutor::execute() and
 * must outlive all HP dirty scheduler callbacks for that request.
 */
struct DownsampleRequestSink {
  /** @brief Queue of HP-to-RT refresh requests to execute after HP work. */
  std::vector<DownsampleExecutor::Request>& requests;

  /** @brief Mutex protecting requests when scheduler workers append entries. */
  std::mutex& mutex;
};

/**
 * @brief Executes one high-precision dirty node from a prepared HP plan.
 *
 * The executor is intentionally node-scoped: it receives an already selected
 * HpPlanEntry, resolves HP inputs, chooses tiled or monolithic HP execution,
 * commits HP ROI/version metadata, records node events, and optionally queues
 * a later downsample request. It does not build dirty snapshots or decide task
 * ordering.
 *
 * @note The owner must hold graph mutation ownership for the whole call. The
 * downsample request vector is protected because scheduler workers may execute
 * multiple HP node tasks concurrently.
 */
class HighPrecisionDirtyNodeExecutor {
 public:
  /**
   * @brief Constructs a node executor for one HP dirty generation.
   *
   * @param context Borrowed graph/runtime/event/snapshot generation context.
   * @param downsample_sink Shared request queue populated after HP commits.
   * @throws Nothing directly.
   * @note All references are borrowed and must outlive scheduler callbacks
   * created for the same dirty generation.
   */
  HighPrecisionDirtyNodeExecutor(DirtyNodeExecutionContext context,
                                 DownsampleRequestSink downsample_sink);

  /**
   * @brief Runs HP dirty execution for one planned node.
   *
   * @param node Mutable node selected by the outer dirty executor.
   * @param entry HP dirty ROI, extent, and halo metadata for this node.
   * @throws GraphError when dependencies or operators are missing, or when the
   * selected operation fails.
   * @note Empty HP ROIs are valid no-op entries.
   */
  void execute(Node& node, const HpPlanEntry& entry);

 private:
  /**
   * @brief Resolves reusable HP outputs for image dependencies.
   *
   * @param node Node whose inputs are resolved.
   * @return Ready image inputs for HP execution.
   * @throws GraphError from NodeInputResolver when required inputs are missing.
   * @note Missing upstream nodes are reported through the resolver's existing
   * missing-dependency policy.
   */
  ResolvedNodeInputs resolve_inputs(Node& node) const;

  /**
   * @brief Executes the best available HP operation for one node.
   *
   * @param node Node being computed.
   * @param entry HP ROI and extent metadata.
   * @param image_inputs_ready Resolved HP image inputs.
   * @throws GraphError when no HP tiled/monolithic operation exists or an
   * operation returns no output.
   * @note Tiled HP implementations remain preferred over monolithic HP
   * implementations, preserving the previous dirty path selection order.
   */
  void execute_operation(
      Node& node, const HpPlanEntry& entry,
      const std::vector<const NodeOutput*>& image_inputs_ready) const;

  /**
   * @brief Ensures the HP cache image buffer can receive tiled dirty output.
   *
   * @param node Node whose HP cache is allocated.
   * @param entry HP output extent.
   * @param image_inputs_ready Resolved inputs used to infer format.
   * @return Mutable HP image buffer matching the planned extent and format.
   * @throws GraphError or std::bad_alloc if allocation fails.
   * @note Existing HP data payloads remain owned by the same NodeOutput object.
   */
  ImageBuffer& ensure_hp_buffer(
      Node& node, const HpPlanEntry& entry,
      const std::vector<const NodeOutput*>& image_inputs_ready) const;

  /**
   * @brief Runs a tiled HP implementation into the selected HP ROI.
   *
   * @param node Node being computed.
   * @param tile_fn Tiled HP operation implementation.
   * @param entry HP ROI, extent, and halo metadata.
   * @param image_inputs_ready Resolved HP image inputs.
   * @throws GraphError or operation exceptions from NodeExecutor.
   * @note Scheduler tile trace events are emitted before execution to mirror
   * the previous combined dirty executor.
   */
  void execute_tiled(
      Node& node, const TileOpFunc& tile_fn, const HpPlanEntry& entry,
      const std::vector<const NodeOutput*>& image_inputs_ready,
      ImageBuffer& hp_buffer) const;

  /**
   * @brief Runs a monolithic HP implementation and stores its output.
   *
   * @param node Node being computed.
   * @param mono_fn Monolithic HP operation implementation.
   * @param image_inputs_ready Resolved HP image inputs.
   * @throws GraphError if the operation produces no output.
   * @note Monolithic HP fallback preserves the previous dirty update behavior.
   */
  void execute_monolithic(
      Node& node, const MonolithicOpFunc& mono_fn,
      const std::vector<const NodeOutput*>& image_inputs_ready) const;

  /**
   * @brief Commits HP ROI, version, source generation, and node event state.
   *
   * @param node Node whose HP cache was updated.
   * @param entry HP ROI and extent used for metadata.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @throws std::bad_alloc if event storage grows and allocation fails.
   * @note HP ROI metadata stays in HP coordinates and merges with previous
   * dirty ROI metadata for the node.
   */
  void commit_node(Node& node, const HpPlanEntry& entry, bool dirty_source);

  /**
   * @brief Queues a later HP-to-RT refresh when scheduler runtime is present.
   *
   * @param node Node whose committed HP output should be downsampled.
   * @param entry HP ROI represented by the committed output.
   * @throws std::bad_alloc if the shared request vector grows.
   * @note Inline dirty execution intentionally leaves this queue empty to
   * preserve the previous post-split behavior.
   */
  void queue_downsample_request(const Node& node, const HpPlanEntry& entry);

  /**
   * @brief Checks and logs stale dirty source generations.
   *
   * @param node Node being considered for HP source execution.
   * @param dirty_source Whether the node is a source boundary.
   * @return True when execution should be skipped.
   * @throws Any exception propagated by GraphRuntime::log_event.
   * @note Non-source nodes are never skipped by source generation metadata.
   */
  bool should_skip_node(const Node& node, bool dirty_source) const;

  /**
   * @brief Returns the request-local mutex for one graph node.
   *
   * @param node_id Node id whose shared dirty output state will be touched.
   * @return Mutex protecting that node's dirty cache state.
   * @throws std::out_of_range when the dirty plan did not allocate a lock for
   * the node id, indicating inconsistent task materialization.
   * @note Locks are per request, so independent HP/RT dirty graphs never share
   * mutex state across intents.
   */
  std::mutex& node_mutex(int node_id) const;

  /** @brief Borrowed graph used for dependencies and tiled execution. */
  GraphModel& graph_;

  /** @brief Optional runtime for trace and stale-generation events. */
  GraphRuntime* runtime_;

  /** @brief Event sink for HP dirty node updates. */
  GraphEventService& events_;

  /** @brief Dirty snapshot that marks source boundary nodes. */
  const DirtyRegionSnapshot& snapshot_;

  /** @brief Dirty generation used to detect stale source callbacks. */
  uint64_t dirty_generation_;

  /** @brief Shared queue of HP-to-RT refresh requests. */
  std::vector<DownsampleExecutor::Request>& downsample_requests_;

  /** @brief Mutex protecting the shared downsample request queue. */
  std::mutex& downsample_requests_mutex_;

  /** @brief Per-node dirty cache locks borrowed from the request executor. */
  DirtyNodeMutexMap& node_mutexes_;
};

/**
 * @brief Executes one real-time dirty node from a prepared RT plan.
 *
 * The executor owns the RT node-level work: resolving transient inputs,
 * selecting an RT operator with HP fallback, allocating proxy buffers, running
 * tiled or monolithic execution, and committing RT ROI/version metadata. It
 * does not build dirty snapshots or decide source-first task ordering.
 *
 * @note RT output remains transient interactive state and is never promoted to
 * reusable high-precision cache authority.
 */
class RealTimeDirtyNodeExecutor {
 public:
  /**
   * @brief Constructs a node executor for one RT dirty generation.
   *
   * @param context Borrowed graph/runtime/event/snapshot generation context.
   * @throws Nothing directly.
   * @note References are borrowed and must outlive scheduler callbacks created
   * for the same dirty generation.
   */
  explicit RealTimeDirtyNodeExecutor(DirtyNodeExecutionContext context);

  /**
   * @brief Runs RT dirty execution for one planned node.
   *
   * @param node Mutable node selected by the outer dirty executor.
   * @param entry RT dirty ROI, HP ROI, extent, and halo metadata.
   * @throws GraphError when dependencies or operators are missing, or when the
   * selected operation fails.
   * @note Empty RT ROIs are valid no-op entries.
   */
  void execute(Node& node, const RtPlanEntry& entry);

 private:
  /**
   * @brief Resolves transient RT outputs for image dependencies.
   *
   * @param node Node whose inputs are resolved.
   * @return Ready image inputs for RT execution.
   * @throws GraphError from NodeInputResolver when required inputs are missing.
   * @note RT dependency lookup uses interactive cache state only.
   */
  ResolvedNodeInputs resolve_inputs(Node& node) const;

  /**
   * @brief Resolves the operation used for RT dirty execution.
   *
   * @param node Node whose operation implementation is requested.
   * @return RT implementation, or HP implementation as fallback.
   * @throws Nothing directly.
   * @note Returning nullopt lets execute() raise the same NoOperation error
   * message as the previous combined dirty executor.
   */
  std::optional<OpRegistry::OpVariant> resolve_operation(
      const Node& node) const;

  /**
   * @brief Ensures the RT proxy buffer matches the planned RT extent.
   *
   * @param node Node whose RT cache is allocated.
   * @param entry RT extent metadata.
   * @param image_inputs_ready Resolved inputs used to infer format.
   * @return Mutable RT image buffer matching the planned extent and format.
   * @throws GraphError or std::bad_alloc if allocation fails.
   * @note HP cache shape is used as a final format hint when RT cache and
   * inputs do not carry concrete image metadata.
   */
  ImageBuffer& ensure_rt_buffer(
      Node& node, const RtPlanEntry& entry,
      const std::vector<const NodeOutput*>& image_inputs_ready) const;

  /**
   * @brief Executes the selected RT or HP-fallback operation.
   *
   * @param node Node being computed.
   * @param entry RT dirty ROI and extent metadata.
   * @param image_inputs_ready Resolved RT image inputs.
   * @param rt_buffer Destination RT proxy buffer.
   * @param op_variant Selected monolithic or tiled operation.
   * @throws GraphError wrapping OpenCV and standard exceptions with node id
   * context, preserving previous RT dirty error reporting.
   * @note GraphError exceptions from the operation are rethrown unchanged.
   */
  void execute_operation(
      Node& node, const RtPlanEntry& entry,
      const std::vector<const NodeOutput*>& image_inputs_ready,
      ImageBuffer& rt_buffer, const OpRegistry::OpVariant& op_variant) const;

  /**
   * @brief Runs a monolithic operation and copies the affected RT ROI.
   *
   * @param node Node being computed.
   * @param entry RT dirty ROI and extent metadata.
   * @param image_inputs_ready Resolved RT image inputs.
   * @param rt_buffer Destination RT proxy buffer.
   * @param mono_fn Monolithic operation implementation.
   * @throws OpenCV exceptions from Mat conversion, resize, ROI slicing, or
   * copy.
   * @note Non-image payloads are copied even when the image buffer is empty.
   */
  void execute_monolithic(
      Node& node, const RtPlanEntry& entry,
      const std::vector<const NodeOutput*>& image_inputs_ready,
      ImageBuffer& rt_buffer, const MonolithicOpFunc& mono_fn) const;

  /**
   * @brief Copies a monolithic image result into the planned RT ROI.
   *
   * @param result Operation result produced by a monolithic implementation.
   * @param entry RT dirty ROI and extent metadata.
   * @param rt_buffer Destination RT proxy buffer.
   * @throws OpenCV exceptions from Mat conversion, resize, ROI slicing, or
   * copy.
   * @note Full-frame monolithic output is resized to the planned RT extent when
   * necessary before the dirty ROI is copied.
   */
  void copy_monolithic_image_roi(const NodeOutput& result,
                                 const RtPlanEntry& entry,
                                 ImageBuffer& rt_buffer) const;

  /**
   * @brief Runs a tiled operation into the selected RT ROI.
   *
   * @param node Node being computed.
   * @param tile_fn Tiled operation implementation.
   * @param entry RT dirty ROI, extent, and halo metadata.
   * @param image_inputs_ready Resolved RT image inputs.
   * @param rt_buffer Destination RT proxy buffer.
   * @throws GraphError or operation exceptions from NodeExecutor.
   * @note Scheduler tile trace events are emitted after tiled execution, as in
   * the previous combined dirty executor.
   */
  void execute_tiled(Node& node, const TileOpFunc& tile_fn,
                     const RtPlanEntry& entry,
                     const std::vector<const NodeOutput*>& image_inputs_ready,
                     ImageBuffer& rt_buffer) const;

  /**
   * @brief Commits RT ROI, version, source generation, and node event state.
   *
   * @param node Node whose RT cache was updated.
   * @param entry HP-space ROI and extent used for inspection metadata.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @throws std::bad_alloc if event storage grows and allocation fails.
   * @note RT ROI metadata stays in HP coordinates for frontend/debug
   * consistency.
   */
  void commit_node(Node& node, const RtPlanEntry& entry, bool dirty_source);

  /**
   * @brief Checks and logs stale dirty source generations.
   *
   * @param node Node being considered for RT source execution.
   * @param dirty_source Whether the node is a source boundary.
   * @return True when execution should be skipped.
   * @throws Any exception propagated by GraphRuntime::log_event.
   * @note Non-source nodes are never skipped by source generation metadata.
   */
  bool should_skip_node(const Node& node, bool dirty_source) const;

  /**
   * @brief Returns the request-local mutex for one graph node.
   *
   * @param node_id Node id whose shared RT cache state will be touched.
   * @return Mutex protecting that node's dirty cache state.
   * @throws std::out_of_range when the dirty plan did not allocate a lock for
   * the node id.
   * @note The lock protects graph-node mutation, not operation execution.
   */
  std::mutex& node_mutex(int node_id) const;

  /** @brief Borrowed graph used for dependencies and tiled execution. */
  GraphModel& graph_;

  /** @brief Optional runtime for trace and stale-generation events. */
  GraphRuntime* runtime_;

  /** @brief Event sink for RT dirty node updates. */
  GraphEventService& events_;

  /** @brief Dirty snapshot that marks source boundary nodes. */
  const DirtyRegionSnapshot& snapshot_;

  /** @brief Dirty generation used to detect stale source callbacks. */
  uint64_t dirty_generation_;

  /** @brief Per-node dirty cache locks borrowed from the request executor. */
  DirtyNodeMutexMap& node_mutexes_;
};

}  // namespace ps::compute
