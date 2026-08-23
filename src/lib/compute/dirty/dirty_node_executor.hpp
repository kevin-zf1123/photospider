#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "compute/dirty/dirty_execution_common.hpp"
#include "compute/dirty/dirty_region_planner.hpp"
#include "compute/dirty/dirty_write_buffers.hpp"
#include "compute/dirty/downsample_executor.hpp"
#include "compute/dirty/node_input_resolver.hpp"

namespace ps {
class GraphEventService;
class GraphModel;
class GraphRuntime;
}  // namespace ps

namespace ps::compute {

class ComputeRunLease;
class ExecutionService;
class StabilizedDirtyParameters;
class TiledInputContext;

/**
 * @brief Immutable operation/device snapshot for one dirty-plan node.
 *
 * The dirty executor resolves this value before service admission so callback
 * execution cannot observe a different registry revision or device inventory.
 * The callable copy retains any plugin DSO lease carried by the registry
 * wrapper.
 *
 * @throws std::bad_alloc when copying callable ownership allocates.
 * @note The selected device names the private execution lane used by every
 * task materialized for the node. CPU fallback remains explicit.
 */
struct DirtyResolvedOperation {
  /** @brief Selected monolithic or tiled callable snapshot. */
  OpRegistry::OpVariant operation;

  /** @brief Device whose private lane must execute the callable. */
  DeviceBackend device = DeviceBackend::CPU;

  /** @brief Nonzero registry revision of the exact selected implementation. */
  std::uint64_t implementation_identity = 0U;

  /**
   * @brief Metadata frozen coherently with the selected callable and identity.
   * @note Execution must not query the registry again for tile, resource, or
   * concurrency behavior.
   */
  OpMetadata metadata;

  /**
   * @brief Exact callback-free output authority frozen by graph planning.
   * @note Dirty execution validates provider results against this value before
   * staging and again before formal HP/RT publication.
   */
  PlannedOutputAuthority output_authority;

  /**
   * @brief Dirty-ROI callback frozen with the exact selected implementation.
   * @note Tiled execution uses this copy instead of consulting the mutable
   * operation-level registry callback after admission.
   */
  std::optional<DirtyRoiPropFunc> dirty_propagator;

  /**
   * @brief Pure tiled output inference frozen with the exact implementation.
   * @note Tiled allocation invokes this callback before Host binding creation
   * and provider entry. Absence selects the conservative no-optional-facts
   * fallback and never borrows an operation-level sibling policy.
   */
  std::optional<TiledOutputInferenceFunc> tiled_output_inference;
};

/** @brief Node-id index of immutable dirty operation/device snapshots. */
using DirtyResolvedOperationMap = std::unordered_map<
    int, DirtyResolvedOperation>;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Borrowed execution context shared by HP and RT dirty node executors.
 *
 * The context groups graph access, optional execution runtime, event recording,
 * dirty-source membership, and generation metadata for one dirty update. It is
 * an aggregate so call sites can construct it without introducing a long
 * constructor parameter list.
 *
 * @note All references are borrowed for execution callbacks created by the
 * owning dirty update executor. The context must not outlive the prepared
 * dirty plan, transaction synchronization owner, or runtime wait for that
 * generation.
 */
struct DirtyNodeExecutionContext {
  /** @brief Graph used for dependency lookup, tiled execution, and commits. */
  GraphModel& graph;

  /** @brief Optional runtime for execution trace and stale-generation events.
   */
  GraphRuntime* runtime;

  /** @brief Event sink for HP/RT dirty node update events. */
  GraphEventService& events;

  /** @brief Dirty snapshot that marks source boundary nodes. */
  const DirtyRegionSnapshot& snapshot;

  /** @brief Frozen operation/device snapshots indexed by graph node id. */
  const DirtyResolvedOperationMap& resolved_operations;

  /** @brief Dirty generation used to reject stale source callbacks. */
  uint64_t dirty_generation;

  /** @brief Per-node synchronization shared by concurrent dirty siblings. */
  DirtyNodeSynchronization& node_synchronization;

  /**
   * @brief Optional immutable parameter snapshot shared by HP/RT siblings.
   * @note HP normally resolves the imported write-buffer copy; RT uses this
   * snapshot only for parameter edges and never for image-edge domain data.
   */
  const StabilizedDirtyParameters* stabilized_parameters = nullptr;

  /**
   * @brief Optional borrowed Run lease for node/tile cancellation observation.
   * @note The owning dirty executor keeps the pointed-to lifecycle lease alive
   * through synchronous task settlement; this context never retains it.
   */
  const ComputeRunLease* run_lease = nullptr;

  /**
   * @brief Optional process authority for direct provider admission.
   * @note Physical service workers leave this null because their ready-entry
   * gate and resource grant already own the same authority. Inline and
   * task-runtime paths supply it together with run_lease.
   */
  ExecutionService* direct_execution_service = nullptr;

  /**
   * @brief Whether RT dirty sources use exact factor-four box averaging.
   * @note HP execution ignores this value. It is true only for the private
   * progressive request path and never changes downstream operation kernels.
   */
  bool exact_factor_four_preview = false;

  /**
   * @brief Executable tiled-task counts keyed by node for node-level sealing.
   * @note Product execution supplies this request-owned map. Null is reserved
   * for direct single-task tests/callers and implies one tiled task per node.
   * The map outlives every executor callback and is immutable after physical
   * admission.
   */
  const std::unordered_map<int, std::size_t>* tiled_task_counts = nullptr;
};

/**
 * @brief Executes one high-precision dirty node from a prepared HP plan.
 *
 * The executor is intentionally node-scoped: it receives an already selected
 * HpPlanEntry, resolves HP inputs, chooses tiled or monolithic HP execution,
 * writes output/ROI/version metadata into a request-local HP write buffer, and
 * records node events. It does not build dirty snapshots, commit GraphModel, or
 * decide task ordering.
 *
 * @note Worker execution never mutates GraphModel HP cache directly. The owner
 * commits the write buffer after the sibling commit gate permits original
 * graph mutation.
 */
class HighPrecisionDirtyNodeExecutor {
 public:
  /**
   * @brief Constructs a node executor for one HP dirty generation.
   *
   * @param context Borrowed graph/runtime/event/snapshot generation context.
   * @param hp_write_buffer Request-local HP output buffer committed by the
   * owning HighPrecisionDirtyExecutor after dirty work completes.
   * @throws Nothing directly.
   * @note All references are borrowed and must outlive execution callbacks
   * created for the same dirty generation.
   */
  HighPrecisionDirtyNodeExecutor(
      DirtyNodeExecutionContext context,
      HighPrecisionDirtyWriteBuffer& hp_write_buffer);

  /**
   * @brief Runs HP dirty execution for one planned node.
   *
   * @param node Mutable node selected by the outer dirty executor.
   * @param entry HP dirty ROI, extent, and halo metadata for this node.
   * @return Nothing.
   * @throws GraphError when dependencies or operators are missing, or when the
   * selected operation fails, including accepted Run cancellation.
   * @note Empty logical Regions and stale dirty-source generations are valid
   * no-op entries after the initial cancellation observation. TensorSlice
   * entries intentionally retain an empty derived roi_hp. Tiled providers
   * observe before every tile; a monolithic provider already entered is
   * non-preemptible. Cancellation observed after provider return suppresses
   * ROI/version/event staging, leaving any partial write-buffer data
   * request-local for outer failure cleanup.
   */
  void execute(Node& node, const HpPlanEntry& entry);

 private:
  /**
   * @brief Resolves staged or committed HP outputs for image dependencies.
   *
   * @param node Node whose inputs are resolved.
   * @return Ready image inputs for HP execution.
   * @throws GraphError from NodeInputResolver when required inputs are missing.
   * @note Lookup checks the current HP write buffer first so downstream work
   * can consume upstream staged output before GraphModel commit.
   */
  ResolvedNodeInputs resolve_inputs(Node& node) const;

  /**
   * @brief Executes the frozen HP operation for one node.
   *
   * @param node Node being computed.
   * @param entry HP ROI and extent metadata.
   * @param image_inputs_ready Resolved HP image inputs.
   * @param operation Planning-time selected operation and metadata snapshot.
   * @return Nothing.
   * @throws std::bad_alloc when operation execution or staging exhausts
   * memory.
   * @throws GraphError when an operation returns no output.
   * @note Selection and device routing already completed before admission.
   * The tiled branch prepares one normalized context before output-buffer
   * creation or plan freezing and retains it through all synchronous tiles.
   */
  void execute_operation(
      Node& node, const HpPlanEntry& entry,
      const std::vector<const NodeOutput*>& image_inputs_ready,
      const DirtyResolvedOperation& operation) const;

  /**
   * @brief Runs a tiled HP implementation through checked Host grants.
   *
   * @param node Node being computed.
   * @param tile_fn Tiled HP operation implementation.
   * @param entry HP ROI, extent, and halo metadata.
   * @param operation Exact selected implementation metadata and ROI callback.
   * @param input_context Exact normalized inputs already used to freeze the
   * output plan.
   * @param output_binding Open request-local destination binding.
   * @return Nothing.
   * @throws GraphError or operation exceptions from NodeExecutor.
   * @note Execution tile trace events are emitted before execution to mirror
   * the previous combined dirty executor. A supplied lifecycle lease is
   * observed before every tile callback enters provider work.
   */
  void execute_tiled(Node& node, const TileOpFunc& tile_fn,
                     const HpPlanEntry& entry,
                     const DirtyResolvedOperation& operation,
                     const TiledInputContext& input_context,
                     HostOutputBinding& output_binding) const;

  /**
   * @brief Runs a monolithic HP implementation and stores its output.
   *
   * @param node Node being computed.
   * @param entry Exact logical Region selected by HP planning.
   * @param mono_fn Monolithic HP operation implementation.
   * @param operation Exact frozen identity and scheduling metadata.
   * @param image_inputs_ready Resolved HP image inputs.
   * @throws GraphError if the operation produces no output.
   * @note The exact core Region bridge stages selected bytes through the HP
   *       write buffer so prior valid coordinates survive a partial result.
   *       Generic monolithic callbacks preserve complete-output replacement
   *       behavior.
   */
  void execute_monolithic(
      Node& node, const HpPlanEntry& entry, const MonolithicOpFunc& mono_fn,
      const DirtyResolvedOperation& operation,
      const std::vector<const NodeOutput*>& image_inputs_ready) const;

  /**
   * @brief Stages HP ROI, version, source generation, and node event state.
   *
   * @param node Node whose staged HP cache was updated.
   * @param entry HP ROI and extent used for metadata.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @param tiled_operation Whether this task wrote the shared tiled binding.
   * @throws std::bad_alloc if event storage grows and allocation fails.
   * @note HP ROI metadata stays in HP coordinates and is committed later by
   * HighPrecisionDirtyExecutor.
   */
  void commit_node(Node& node, const HpPlanEntry& entry, bool dirty_source,
                   bool tiled_operation);

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
   * @note Concurrent HP/RT siblings of one RealTimeUpdate share this mutex,
   * while independent requests and Graphs never share synchronization state.
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

  /** @brief Frozen operation/device snapshots for the current dirty plan. */
  const DirtyResolvedOperationMap& resolved_operations_;

  /** @brief Dirty generation used to detect stale source callbacks. */
  uint64_t dirty_generation_;

  /** @brief Request-local buffer receiving HP output and metadata writes. */
  HighPrecisionDirtyWriteBuffer& hp_write_buffer_;

  /** @brief Per-node critical sections borrowed from the dirty transaction. */
  DirtyNodeSynchronization& node_synchronization_;

  /** @brief Optional borrowed lifecycle lease for cooperative observations. */
  const ComputeRunLease* run_lease_ = nullptr;

  /** @brief Optional process authority for direct provider admission. */
  ExecutionService* direct_execution_service_ = nullptr;

  /** @brief Frozen executable tiled-task counts, or null for direct callers. */
  const std::unordered_map<int, std::size_t>* tiled_task_counts_ = nullptr;
};

/**
 * @brief Executes one real-time dirty node from a prepared RT plan.
 *
 * The executor owns the RT node-level work: resolving transient inputs,
 * selecting an RT operator with HP fallback, allocating staged proxy buffers,
 * running tiled or monolithic execution, and recording RT ROI/version metadata
 * into a request-local write buffer. It does not build dirty snapshots, decide
 * source-first task ordering, or commit staged output to RealtimeProxyGraph.
 *
 * @note RT output remains in RealtimeProxyGraph and is never promoted to
 * GraphModel or reusable high-precision cache authority.
 */
class RealTimeDirtyNodeExecutor {
 public:
  /**
   * @brief Constructs a node executor for one RT dirty generation.
   *
   * @param context Borrowed graph/runtime/event/snapshot generation context.
   * @param proxy_graph Committed RT proxy graph used for upstream fallback and
   * stale source generation checks.
   * @param rt_write_buffer Request-local RT output buffer committed to
   * RealtimeProxyGraph after all dirty tasks complete.
   * @throws Nothing directly.
   * @note References are borrowed and must outlive execution callbacks created
   * for the same dirty generation.
   */
  RealTimeDirtyNodeExecutor(DirtyNodeExecutionContext context,
                            RealtimeProxyGraph& proxy_graph,
                            RealtimeProxyWriteBuffer& rt_write_buffer);

  /**
   * @brief Runs RT dirty execution for one planned node.
   *
   * @param node Mutable node selected by the outer dirty executor.
   * @param entry RT dirty ROI, HP ROI, extent, and halo metadata.
   * @return Nothing.
   * @throws GraphError when dependencies or operators are missing, or when the
   * selected operation fails, including accepted Run cancellation.
   * @note Empty RT ROIs and stale dirty-source generations are valid no-op
   * entries after the initial cancellation observation. Tiled providers
   * observe before every tile; a monolithic provider already entered is
   * non-preemptible. Cancellation observed after provider return suppresses
   * ROI/version/event staging, leaving any partial proxy write-buffer data
   * request-local for outer failure cleanup. The tiled branch prepares one
   * normalized context before plan freezing or Host allocation and retains it
   * through every synchronous callback.
   */
  void execute(Node& node, const RtPlanEntry& entry);

 private:
  /**
   * @brief Resolves staged or committed RT outputs for image dependencies.
   *
   * @param node Node whose inputs are resolved.
   * @return Ready image inputs for RT execution.
   * @throws GraphError from NodeInputResolver when required inputs are missing.
   * @note Dependency lookup first checks the request-local RT write buffer,
   * then committed proxy graph state, then original graph HP output as a
   * serial compatibility fallback.
   */
  ResolvedNodeInputs resolve_inputs(Node& node) const;

  /**
   * @brief Copies a monolithic Value result through one checked RT grant.
   *
   * @param result Operation result produced by a monolithic implementation.
   * @param entry RT dirty ROI and extent metadata.
   * @param output_binding Open Host binding for the RT proxy Value.
   * @param exact_factor_four_source Whether to use the frozen aligned box
   * average instead of ordinary build-selected resize.
   * @return Nothing.
   * @throws GraphError, standard, or image-processing exceptions from Value
   * projection, resize/downsample scratch work, grant issuance, or copy.
   * @throws std::invalid_argument or std::out_of_range when exact factor-four
   * geometry, format, storage separation, or ROI constraints are invalid.
   * @note Resize/downsample dense Values are callback-local read/scratch
   * projections only. The binding remains the sole mutable RT authority and is
   * sealed by the caller after this function retires its grant.
   */
  void copy_monolithic_image_roi(const NodeOutput& result,
                                 const RtPlanEntry& entry,
                                 HostOutputBinding& output_binding,
                                 bool exact_factor_four_source) const;

  /**
   * @brief Runs a tiled operation into the selected RT ROI.
   *
   * @param node Node being computed.
   * @param tile_fn Tiled operation implementation.
   * @param entry RT dirty ROI, extent, and halo metadata.
   * @param operation Exact selected implementation metadata and ROI callback.
   * @param input_context Exact normalized inputs already used to freeze the
   * output plan.
   * @param output_binding Open request-local destination binding.
   * @return Nothing.
   * @throws GraphError or operation exceptions from NodeExecutor.
   * @note Execution tile trace events are emitted after tiled execution, as in
   * the previous combined dirty executor. A supplied lifecycle lease is
   * observed before every tile callback enters provider work.
   */
  void execute_tiled(Node& node, const TileOpFunc& tile_fn,
                     const RtPlanEntry& entry,
                     const DirtyResolvedOperation& operation,
                     const TiledInputContext& input_context,
                     HostOutputBinding& output_binding) const;

  /**
   * @brief Records RT ROI, version, source generation, and node event state.
   *
   * @param node Node whose staged RT output was updated.
   * @param entry HP-space ROI and extent used for inspection metadata.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @param tiled_operation Whether this task wrote the shared tiled binding.
   * @throws std::bad_alloc if event storage grows and allocation fails.
   * @note RT ROI metadata is staged in HP coordinates for frontend/debug
   * consistency and committed by the owning RealTimeDirtyExecutor.
   */
  void commit_node(Node& node, const RtPlanEntry& entry, bool dirty_source,
                   bool tiled_operation);

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

  /** @brief Frozen operation/device snapshots for the current dirty plan. */
  const DirtyResolvedOperationMap& resolved_operations_;

  /** @brief Dirty generation used to detect stale source callbacks. */
  uint64_t dirty_generation_;

  /** @brief Immutable HP-stabilized values used only by parameter edges. */
  const StabilizedDirtyParameters* stabilized_parameters_ = nullptr;

  /** @brief Committed proxy graph used for RT fallback and source metadata. */
  RealtimeProxyGraph& proxy_graph_;

  /** @brief Request-local buffer that receives RT output and metadata writes.
   */
  RealtimeProxyWriteBuffer& rt_write_buffer_;

  /** @brief Per-node critical sections borrowed from the dirty transaction. */
  DirtyNodeSynchronization& node_synchronization_;

  /** @brief Optional borrowed lifecycle lease for cooperative observations. */
  const ComputeRunLease* run_lease_ = nullptr;

  /** @brief Optional process authority for direct provider admission. */
  ExecutionService* direct_execution_service_ = nullptr;

  /** @brief Frozen exact RT source-normalization selector. */
  bool exact_factor_four_preview_ = false;

  /** @brief Frozen executable tiled-task counts, or null for direct callers. */
  const std::unordered_map<int, std::size_t>* tiled_task_counts_ = nullptr;
};

}  // namespace ps::compute
