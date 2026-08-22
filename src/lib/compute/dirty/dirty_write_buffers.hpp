#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "compute/dirty/downsample_executor.hpp"
#include "compute/dirty/realtime_proxy_graph.hpp"
#include "compute/dispatch/task_graph_planning.hpp"
#include "core/host_output_authorization.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/region.hpp"

namespace ps::compute {

/**
 * @brief Request-local staging buffer for HP dirty writes.
 *
 * HighPrecisionDirtyWriteBuffer owns per-node HP outputs, Region metadata,
 * version counters, and dirty-source generation updates produced by dirty
 * worker tasks. The owning executor commits the staged data into GraphModel
 * only after the RT sibling commit gate allows original-graph mutation.
 *
 * @note Existing sealed Values are retained immutably. Tiled ROI work allocates
 * a fresh Host binding, optionally seeds it from those bytes, and replaces the
 * staged Value only after all grants retire and seal succeeds. No mutable
 * mutable image staging or duplicate revision authority exists.
 */
class HighPrecisionDirtyWriteBuffer {
 public:
  /**
   * @brief Constructs an HP staging buffer.
   *
   * @param seed_existing_outputs Whether ensure_output() should seed from
   * existing GraphModel HP cache. Force-recache HP dirty requests pass false
   * only after the executor has expanded planning to the full HP frame.
   * @throws Nothing.
   */
  explicit HighPrecisionDirtyWriteBuffer(bool seed_existing_outputs = true);

  /**
   * @brief Returns staged HP output for one node when available.
   *
   * @param node_id Graph node id to inspect.
   * @return Pointer to staged output, or nullptr when the node has not staged
   * output in this request.
   * @throws Nothing directly.
   * @note The pointer remains valid until buffer destruction or commit.
   */
  const NodeOutput* find_output(int node_id) const;

  /**
   * @brief Copies one staged HP output under the buffer mutex.
   * @param node_id Graph node id to inspect.
   * @return Complete copied output, or nullopt when no output is staged.
   * @throws std::bad_alloc when copied metadata storage cannot allocate.
   * @note Fence continuations use this stable snapshot instead of retaining a
   * pointer after the mutex is released. Immutable Value identity is retained.
   */
  std::optional<NodeOutput> copy_output(int node_id) const;

  /**
   * @brief Checks whether staged HP output exists for one node.
   *
   * @param node_id Graph node id to inspect.
   * @return True when find_output(node_id) returns non-null.
   * @throws Nothing directly.
   */
  bool has_output(int node_id) const;

  /**
   * @brief Ensures a writable staged HP output exists for one node.
   *
   * @param node Graph node whose current HP state seeds the staged entry.
   * @return Mutable staged output owned by this buffer.
   * @throws GraphError when existing output contains forbidden compatibility
   * staging; may throw std::bad_alloc while allocating map entries/metadata.
   * @note When constructed with seed_existing_outputs=false, the staged output
   * starts empty even if the graph already has HP cache; callers must then
   * execute a full-output HP plan before commit.
   */
  NodeOutput& ensure_output(const Node& node);

  /**
   * @brief Returns the sole request binding for one HP tiled node.
   *
   * @param node Graph node whose immutable output state seeds the binding.
   * @param plan Complete frozen plan derived before any producer callback.
   * @param expected_task_count Positive executable tiled-task count frozen by
   * request plan/entry geometry for this node.
   * @return Stable open binding shared by every disjoint task for this node.
   * @throws std::invalid_argument when the count is zero or a later task
   * supplies a different plan/count.
   * @throws std::logic_error when the retained binding is no longer available
   * for task execution.
   * @throws std::bad_alloc, std::overflow_error, ReadyFenceAccessError,
   * BufferAccessError, or grant lifecycle exceptions from allocation/seeding.
   * @note The first caller allocates and optionally seeds exactly one binding
   * under the write-buffer mutex. Later task callbacks issue disjoint grants
   * through the same binding. The last executable task seals the binding;
   * commit_to_graph() rejects any undrained state. Failure/cancellation
   * destroys it unpublished. The returned reference remains valid until that
   * commit or buffer destruction, neither of which may race active tasks.
   */
  HostOutputBinding& ensure_tiled_output_binding(
      const Node& node, DenseImageOutputPlan plan,
      std::size_t expected_task_count);

  /**
   * @brief Retires one successful HP tiled task and seals at the node join.
   *
   * @param node Graph node whose shared binding received this task's grants.
   * @return Nothing after decrementing the frozen task count; the last task
   * seals the binding and publishes its one immutable staged image Value.
   * @throws std::logic_error when no matching binding/count exists, a task is
   * completed twice, or seal observes an active/missing grant.
   * @throws std::invalid_argument, std::overflow_error, std::bad_alloc, or
   * std::system_error from final Value/NodeOutput publication.
   * @note The call occurs only after every grant issued by this task retired.
   * Task-graph image consumers depend on the complete upstream node task set,
   * so none can observe staging before this exact node-level seal point.
   */
  void complete_tiled_task(const Node& node);

  /**
   * @brief Stages one Region-aware monolithic result with byte preservation.
   *
   * @param node Graph node whose request-local output is replaced or merged.
   * @param output Fresh complete-shape result whose selected coordinates are
   * trusted for this update.
   * @param updated_region Exact logical coordinates computed by `output`.
   * @return Nothing.
   * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc from
   * dense Region validation, byte merging, or immutable Value publication.
   * @note When existing staged validity and compatible bytes are available,
   * selected elements come from `output` while all other bytes are preserved.
   * The first seeded rank-general merge may read the node's immutable Value
   * because its mutable compatibility clone intentionally drops that identity.
   * If safe merging is impossible, prior validity is discarded before the
   * fresh result is staged, so mark_updated() can publish only
   * `updated_region` rather than a false union.
   */
  void stage_region_output(const Node& node, NodeOutput output,
                           const RegionSet& updated_region);

  /**
   * @brief Imports one immutable HP preflight result into request staging.
   *
   * @param node Graph node whose final HP state may be committed.
   * @param output Preflight output copied into request-local ownership.
   * @param hp_version Version to publish after complete request success.
   * @param hp_region Logical HP validity represented by output, when known.
   * @param dirty_source_generation Shared generation for preflight closure
   * roots, otherwise nullopt.
   * @return Nothing.
   * @throws std::bad_alloc when output ownership or map storage is copied.
   * @note Import never mutates GraphModel. A later phase may read the staged
   * output, but externally satisfied preflight nodes must not write it again.
   */
  void import_precomputed_output(
      const Node& node, const NodeOutput& output, int hp_version,
      const std::optional<RegionSet>& hp_region,
      std::optional<uint64_t> dirty_source_generation = std::nullopt);

  /**
   * @brief Records HP metadata for one successful dirty node update.
   *
   * @param node Graph node whose staged output was updated.
   * @param region_hp Exact normalized signed logical HP Region represented by
   * this update.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @param dirty_generation Dirty generation committed for source nodes.
   * @return New staged HP version after incrementing.
   * @throws std::logic_error, std::invalid_argument, std::overflow_error, or
   * std::bad_alloc when staged validity or retained output facts cannot be
   * validated.
   * @note Version increments once per dirty task, preserving prior executor
   * semantics while hiding the increment until graph commit. Exact unions are
   * retained when representable, while an already complete Region proof stays
   * complete across a byte-preserving update even if ImageRect and TensorSlice
   * use different domains. Otherwise the fresh exact update replaces prior
   * validity as a safe under-approximation, never as a false superset.
   */
  int mark_updated(const Node& node, const RegionSet& region_hp,
                   bool dirty_source, uint64_t dirty_generation);

  /**
   * @brief Moves staged HP state into the original GraphModel.
   *
   * @param graph Graph receiving HP outputs and metadata.
   * @throws GraphError when a staged node no longer exists.
   * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc when
   * final named-Value validation fails.
   * @note The caller must hold graph.graph_mutex_, and every physical task
   * must already be drained. This method first seals and validates every
   * retained binding before publishing any graph state, then moves all staged
   * outputs. It performs no locking on GraphModel.
   */
  void commit_to_graph(GraphModel& graph,
                       const std::vector<PlannedNodeWork>& planned_work);

  /**
   * @brief Builds HP-to-RT downsample requests from committed staged state.
   *
   * @return Downsample requests carrying signed logical HP Regions and final
   * staged HP versions.
   * @throws std::bad_alloc if result or copied Region storage allocation fails.
   * @note The requests are valid after commit_to_graph() has made HP outputs
   * visible on GraphModel. Exact ImageRect remains logical authority until the
   * DownsampleExecutor observes the committed data-window origin. Empty keeps
   * the legacy full-frame fallback; TensorSlice and Whole validity have no
   * current partial image-only request and are intentionally omitted.
   */
  std::vector<DownsampleExecutor::Request> downsample_requests() const;

  /**
   * @brief Estimates complete Host-owned HP staging structure.
   * @return Checked buffer object, map nodes, and visible output metadata
   * bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note Image pixels and opaque backend/plugin owners are excluded because
   * their allocation capacity remains encapsulated by `BufferHandle`.
   */
  std::uint64_t retained_memory_bytes() const;

  /**
   * @brief Estimates map storage for anticipated entries not yet retained.
   * @param graph Stable graph supplying any HP output that a new entry will
   * deterministically seed.
   * @param anticipated_node_ids Nodes whose current service phase may stage;
   * duplicate ids and ids already present in this buffer are ignored.
   * @return Checked value, tree-linkage, bookkeeping, and minimum visible
   * output-metadata bytes for missing entries only.
   * @throws GraphError when a node is absent or checked arithmetic overflows.
   * @throws std::bad_alloc when temporary deduplication storage cannot grow.
   * @note Seeded visible output metadata is charged from current graph state;
   * otherwise the deterministic empty `NodeOutput` metadata created by
   * ensure_output() is charged. Future operation-produced pixels, named-value
   * growth, and opaque backend/plugin allocations remain excluded.
   */
  std::uint64_t missing_entry_retained_memory_bytes(
      const GraphModel& graph,
      const std::vector<int>& anticipated_node_ids) const;

 private:
  /**
   * @brief Complete staged HP state for one graph node.
   *
   * @note `initialized` tracks whether graph metadata has been captured.
   * `has_output` controls whether commit_to_graph() writes output state.
   */
  struct Entry {
    /** @brief True after graph output/version metadata has been captured. */
    bool initialized = false;
    /** @brief True when commit_to_graph() must publish this node. */
    bool has_output = false;
    /** @brief Immutable named Values and non-payload node output metadata. */
    NodeOutput output;
    /** @brief Exact HP validity known for the staged output. */
    std::optional<RegionSet> hp_region;
    /** @brief Version published only after complete request success. */
    int hp_version = 0;
    /** @brief Dirty-source generation published with successful commit. */
    std::optional<uint64_t> dirty_source_generation;
    /** @brief Sole open Host allocation authority for all tiled tasks. */
    std::optional<HostOutputBinding> tiled_binding;
    /** @brief Frozen number of executable tiled tasks for this node. */
    std::size_t tiled_task_count = 0U;
    /** @brief Successful tiled tasks still required before node-level seal. */
    std::size_t tiled_tasks_remaining = 0U;
  };

  /**
   * @brief Ensures an entry exists and has graph metadata initialized.
   *
   * @param node Graph node used for initial HP metadata.
   * @return Mutable staged entry.
   * @throws GraphError when existing output has compatibility staging.
   * @note The caller must hold mutex_. Existing Region validity is seeded only
   * with an existing output when seed_existing_outputs_ is true; an empty
   * force-recache entry never inherits old validity.
   */
  Entry& ensure_entry_locked(const Node& node);

  /** @brief Whether new entries seed output pixels from GraphModel HP cache. */
  bool seed_existing_outputs_ = true;

  /** @brief Mutex protecting staged entry creation and metadata updates. */
  mutable std::mutex mutex_;

  /** @brief Staged HP entries keyed by original GraphModel node id. */
  std::map<int, Entry> entries_;
};

/**
 * @brief Request-local staging buffer for RT proxy graph writes.
 *
 * RealtimeProxyWriteBuffer owns low-resolution output and RT metadata while RT
 * dirty worker tasks execute. After all RT work drains, the owning executor
 * commits staged state into RealtimeProxyGraph, not into GraphModel.
 *
 * @note Existing committed proxy Values are retained immutably. ROI writes use
 * fresh Host bindings and replace staged state only with sealed Values.
 */
class RealtimeProxyWriteBuffer {
 public:
  /**
   * @brief Constructs an RT proxy staging buffer.
   *
   * @param proxy_graph Committed proxy graph used to seed existing RT state.
   * @param seed_existing_outputs Whether staged entries should seed from the
   * committed proxy graph. Force-recache requests pass false.
   * @throws Nothing.
   */
  explicit RealtimeProxyWriteBuffer(RealtimeProxyGraph& proxy_graph,
                                    bool seed_existing_outputs = true);

  /**
   * @brief Returns staged RT proxy output for one node when available.
   *
   * @param node_id Graph node id to inspect.
   * @return Pointer to staged output, or nullptr when absent.
   * @throws Nothing directly.
   * @note The pointer remains valid until buffer destruction or commit.
   */
  const NodeOutput* find_output(int node_id) const;

  /**
   * @brief Copies one staged RT output under the buffer mutex.
   * @param node_id Original graph node id to inspect.
   * @return Complete copied output, or nullopt when no output is staged.
   * @throws std::bad_alloc when copied metadata storage cannot allocate.
   * @note The snapshot retains immutable Value identity without exposing a
   * pointer across concurrent dirty-task completion.
   */
  std::optional<NodeOutput> copy_output(int node_id) const;

  /**
   * @brief Checks whether staged RT output exists for one node.
   *
   * @param node_id Graph node id to inspect.
   * @return True when find_output(node_id) returns non-null.
   * @throws Nothing directly.
   */
  bool has_output(int node_id) const;

  /**
   * @brief Ensures a writable staged RT proxy output exists for one node.
   *
   * @param node_id Original GraphModel node id.
   * @return Mutable staged output owned by this buffer.
   * @throws GraphError when existing proxy output contains forbidden
   * compatibility staging; may throw std::bad_alloc for map/metadata storage.
   * @note The staged output is seeded from RealtimeProxyGraph only when this
   * buffer was constructed with seed_existing_outputs=true.
   */
  NodeOutput& ensure_output(int node_id);

  /**
   * @brief Publishes one completed monolithic result into proxy staging.
   * @param node_id Original graph node id.
   * @param output Complete result containing only immutable named Values.
   * @param preserved_existing_bytes Whether the new binding retained every
   * byte covered by the prior signed logical HP validity Region.
   * @return Nothing.
   * @throws GraphError when output contains compatibility staging.
   * @throws std::bad_alloc when map or output metadata ownership allocates.
   * @note Failure to preserve prior bytes clears prior validity before the new
   * exact update Region is marked. An active tiled binding is rejected so the
   * node cannot acquire two output authorities; committed proxy state remains
   * unchanged until commit_to_proxy_graph().
   */
  void stage_output(int node_id, NodeOutput output,
                    bool preserved_existing_bytes);

  /**
   * @brief Returns the sole request binding for one RT tiled node.
   *
   * @param node_id Original graph node id whose proxy state seeds the binding.
   * @param plan Complete frozen RT output plan.
   * @param expected_task_count Positive executable tiled-task count frozen by
   * request plan/entry geometry for this node.
   * @return Stable open binding shared by every disjoint RT task for the node.
   * @throws std::invalid_argument when the count is zero or a later task
   * supplies a different plan/count.
   * @throws std::logic_error when monolithic output already replaced the node
   * or the retained binding is no longer open for task execution.
   * @throws std::bad_alloc, std::overflow_error, ReadyFenceAccessError,
   * BufferAccessError, or grant lifecycle exceptions from allocation/seeding.
   * @note Allocation and optional seed happen once under the write-buffer
   * mutex. Task callbacks only borrow the binding to issue disjoint grants;
   * the last executable task seals it, and commit_to_proxy_graph() rejects any
   * undrained state. The reference remains valid until seal, commit, or buffer
   * destruction, none of which may race active tasks.
   */
  HostOutputBinding& ensure_tiled_output_binding(
      int node_id, DenseImageOutputPlan plan, std::size_t expected_task_count);

  /**
   * @brief Retires one successful RT tiled task and seals at the node join.
   *
   * @param node_id Original graph node whose shared binding was written.
   * @return Nothing after decrementing the frozen task count; the last task
   * seals and publishes the staged immutable proxy image Value.
   * @throws std::logic_error for absent/inconsistent count or binding state,
   * duplicate completion, or seal with an active/missing grant.
   * @throws std::invalid_argument, std::overflow_error, std::bad_alloc, or
   * std::system_error from final Value/NodeOutput publication.
   * @note Every task grant must already be retired. Task-graph image consumers
   * wait for the complete upstream node task set, preventing pre-seal reads.
   */
  void complete_tiled_task(int node_id);

  /**
   * @brief Records RT proxy metadata for one successful dirty update.
   *
   * @param node_id Graph node id whose proxy output was updated.
   * @param region_hp Exact normalized signed logical HP ImageRect Region
   * represented by this RT update.
   * @param dirty_source Whether the node is a dirty source boundary.
   * @param dirty_generation Dirty generation committed for source nodes.
   * @return New staged RT proxy version after incrementing.
   * @throws std::invalid_argument when region_hp is not one exact built-in
   * logical ImageRect.
   * @throws std::bad_alloc if the staged entry or Region algebra must allocate.
   * @note No PixelRect projection occurs here; signed logical HP coordinates
   * remain valid independently of the zero-based RT storage ROI.
   */
  int mark_updated(int node_id, const RegionSet& region_hp, bool dirty_source,
                   uint64_t dirty_generation);

  /**
   * @brief Moves all staged RT proxy state into RealtimeProxyGraph.
   *
   * @throws GraphError when proxy output validation fails; may throw
   * std::bad_alloc while committing defensive node entries.
   * @note Every physical task must already be drained. All retained bindings
   * are sealed and validated before any proxy state is published. GraphModel
   * is not read or mutated during this commit.
   */
  void commit_to_proxy_graph(const std::vector<PlannedNodeWork>& planned_work);

  /**
   * @brief Estimates complete Host-owned RT staging structure.
   * @return Checked buffer object, map nodes, and visible output metadata
   * bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note The borrowed committed proxy graph and opaque image/backend payloads
   * are excluded.
   */
  std::uint64_t retained_memory_bytes() const;

  /**
   * @brief Estimates map storage for anticipated entries not yet retained.
   * @param anticipated_node_ids Nodes whose current service phase may stage;
   * duplicate ids and ids already present in this buffer are ignored.
   * @return Checked value, tree-linkage, bookkeeping, and minimum visible
   * output-metadata bytes for missing entries only.
   * @throws GraphError when checked structural arithmetic overflows.
   * @throws std::bad_alloc when temporary deduplication storage cannot grow.
   * @note Seeded visible output metadata is charged from current proxy state;
   * otherwise the deterministic empty `NodeOutput` metadata created by
   * ensure_output() is charged. Future operation-produced pixels, named-value
   * growth, and opaque backend/plugin allocations remain excluded.
   */
  std::uint64_t missing_entry_retained_memory_bytes(
      const std::vector<int>& anticipated_node_ids) const;

 private:
  /**
   * @brief Complete staged RT proxy state for one node id.
   *
   * @note `initialized` records whether committed proxy metadata has been
   * captured. `has_output` controls whether the entry is committed.
   */
  struct Entry {
    /** @brief True after committed proxy metadata has been captured. */
    bool initialized = false;
    /** @brief True when commit_to_proxy_graph() must publish this node. */
    bool has_output = false;
    /** @brief Complete staged proxy metadata and optional named Values. */
    RealtimeProxyGraph::NodeState state;
    /** @brief Sole open Host allocation authority for all tiled RT tasks. */
    std::optional<HostOutputBinding> tiled_binding;
    /** @brief Frozen number of selected RT tiled tasks for this node. */
    std::size_t tiled_task_count = 0U;
    /** @brief Successful RT tiled tasks remaining before node-level seal. */
    std::size_t tiled_tasks_remaining = 0U;
  };

  /**
   * @brief Ensures an entry exists and has proxy metadata initialized.
   *
   * @param node_id Original GraphModel node id.
   * @return Mutable staged entry.
   * @throws GraphError when existing output has compatibility staging.
   * @note The caller must hold mutex_. Existing Region validity is seeded only
   * with an existing proxy output when seed_existing_outputs_ is true.
   */
  Entry& ensure_entry_locked(int node_id);

  /** @brief Committed proxy graph that receives staged RT output. */
  RealtimeProxyGraph& proxy_graph_;

  /** @brief Whether entries seed output pixels from committed proxy state. */
  bool seed_existing_outputs_ = true;

  /** @brief Mutex protecting staged entry creation and metadata updates. */
  mutable std::mutex mutex_;

  /** @brief Staged RT entries keyed by original GraphModel node id. */
  std::map<int, Entry> entries_;
};

}  // namespace ps::compute
