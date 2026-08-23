#include "compute/dispatch/compute_node_task_runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_run.hpp"
#include "compute/dirty/node_executor.hpp"
#include "compute/execution/resource_demand_estimator.hpp"
#include "compute/request/compute_cache_policy.hpp"
#include "compute/request/compute_metrics_recorder.hpp"
#include "core/param_utils.hpp"
#include "graph/graph_cache_service.hpp"
#include "runtime/graph_event_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Finalizes output metadata through the shared metrics recorder.
 *
 * @param output Node output produced by an operation or loaded from cache.
 * @param inputs Image inputs that contributed metadata to the output.
 * @param enable_timing Whether execution timing should be attached.
 * @param execution_ms Measured execution duration in milliseconds.
 * @throws Any exception propagated by ComputeMetricsRecorder.
 * @note This thin wrapper keeps metadata policy centralized while allowing the
 * runner implementation to stay independent from recorder internals.
 */
void finalize_output_metadata(NodeOutput& output,
                              const std::vector<const NodeOutput*>& inputs,
                              bool enable_timing, double execution_ms) {
  ComputeMetricsRecorder::finalize_output_metadata(output, inputs,
                                                   enable_timing, execution_ms);
}

/**
 * @brief Formats a node id with the node name when the graph still contains it.
 *
 * @param graph GraphModel used for node lookup.
 * @param node_id Node id being reported.
 * @return Human-readable context string for error messages.
 * @throws std::bad_alloc if string construction fails.
 * @note Missing nodes are reported by id only so error wrapping remains usable
 * while graph state is being mutated by callers.
 */
std::string node_context(const GraphModel& graph, int node_id) {
  const Node* node = graph.find_node(node_id);
  if (!node) {
    return "node " + std::to_string(node_id);
  }
  return "node " + std::to_string(node_id) + " (" + node->name + ")";
}

/**
 * @brief Creates a compute-stage GraphError exception pointer.
 *
 * @param graph GraphModel used to enrich the node label.
 * @param node_id Node whose operation or dependency resolution failed.
 * @param detail Original exception detail.
 * @return Exception pointer carrying GraphErrc::ComputeError.
 * @throws std::bad_alloc if the wrapped error string cannot be allocated.
 * @note Worker tasks use this helper before rethrowing so runtime exception
 * capture receives a stable, graph-aware error category.
 */
std::exception_ptr compute_failure(const GraphModel& graph, int node_id,
                                   const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Compute stage at " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
}

/**
 * @brief Merges a tile ROI into a planned full output size.
 *
 * @param current Current accumulated output size.
 * @param roi Tile ROI from the immutable task graph.
 * @return Size large enough to contain roi and current.
 * @throws Nothing.
 * @note The planner emits tile ROIs in output coordinates, so the max extents
 * across all tile tasks reconstruct the full node output size.
 */
PixelSize merge_task_extent(const PixelSize& current,
                            const PixelRect& roi) noexcept {
  if (roi.width <= 0 || roi.height <= 0) {
    return current;
  }
  const std::int64_t right = static_cast<std::int64_t>(roi.x) + roi.width;
  const std::int64_t bottom = static_cast<std::int64_t>(roi.y) + roi.height;
  if (right <= 0 || bottom <= 0 || right > std::numeric_limits<int>::max() ||
      bottom > std::numeric_limits<int>::max()) {
    return current;
  }
  return PixelSize{std::max(current.width, static_cast<int>(right)),
                   std::max(current.height, static_cast<int>(bottom))};
}

/**
 * @brief Observes cancellation at one node or tile execution boundary.
 *
 * @param run_lease Optional borrowed request lifecycle lease.
 * @return Nothing when no cancellation owns the Run terminal outcome.
 * @throws GraphError when the matching Run has accepted cancellation.
 * @throws std::system_error when Run-state synchronization fails.
 * @note The exact stable cancellation reason remains owned by ComputeRun and is
 * translated by the outer ComputeService wrapper. This local exception only
 * stops further operation work and cannot replace terminal ownership.
 */
void observe_runner_cancellation(const ComputeRunLease* run_lease) {
  if (run_lease != nullptr && run_lease->observe_cancellation().has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ComputeRun cancelled before node or tile execution.");
  }
}

}  // namespace

/** @copydoc NodeTaskRunner::retained_memory_bytes */
std::uint64_t NodeTaskRunner::retained_memory_bytes() const {
  RetainedMemoryEstimator estimate("NodeTaskRunner");
  estimate.add_objects<NodeTaskRunner>();
  estimate.add_objects<PixelSize>(
      static_cast<std::uint64_t>(planned_output_sizes_.capacity()));
  estimate.add_objects<int>(
      static_cast<std::uint64_t>(tile_task_counts_.capacity()));
  estimate.add_objects<std::atomic<int>>(
      static_cast<std::uint64_t>(completed_tile_counts_.capacity()));
  estimate.add_objects<std::atomic<bool>>(
      static_cast<std::uint64_t>(node_precomputed_.capacity()));
  estimate.add_objects<std::unique_ptr<std::mutex>>(
      static_cast<std::uint64_t>(output_mutexes_.capacity()));
  estimate.add_objects<std::unique_ptr<HostOutputBinding>>(
      static_cast<std::uint64_t>(tile_output_bindings_.capacity()));
  estimate.add_objects<std::unique_ptr<TiledNodePreparationState>>(
      static_cast<std::uint64_t>(tiled_node_preparation_states_.capacity()));
  for (const std::unique_ptr<std::mutex>& mutex : output_mutexes_) {
    if (mutex) {
      estimate.add_objects<std::mutex>();
    }
  }
  for (const std::unique_ptr<TiledNodePreparationState>& state :
       tiled_node_preparation_states_) {
    if (!state) {
      continue;
    }
    estimate.add_objects<TiledNodePreparationState>();
  }
  for (int tile_count : tile_task_counts_) {
    if (tile_count > 0) {
      estimate.add_objects<TiledNodeExecutionContext>();
    }
  }
  return estimate.bytes();
}

/** @copydoc NodeTaskRunner::NodeTaskRunner */
NodeTaskRunner::NodeTaskRunner(NodeTaskRunnerContext context)
    : graph_(context.graph),
      cache_(context.cache),
      events_(context.events),
      task_runtime_(context.task_runtime),
      timing_results_(context.timing_results),
      timing_mutex_(context.timing_mutex),
      execution_order_(context.execution_order),
      id_to_idx_(context.id_to_idx),
      temp_results_(context.temp_results),
      resolved_ops_(context.resolved_ops),
      task_graph_(context.task_graph),
      force_recache_(context.force_recache),
      enable_timing_(context.enable_timing),
      disable_disk_cache_(context.disable_disk_cache),
      benchmark_events_(context.benchmark_events),
      run_lease_(context.run_lease),
      planned_work_(context.planned_work) {
  planned_output_sizes_.assign(execution_order_.size(), PixelSize{});
  tile_task_counts_.assign(execution_order_.size(), 0);
  completed_tile_counts_ =
      std::vector<std::atomic<int>>(execution_order_.size());
  node_precomputed_ = std::vector<std::atomic<bool>>(execution_order_.size());
  tile_output_bindings_.resize(execution_order_.size());
  tiled_node_preparation_states_.reserve(execution_order_.size());
  output_mutexes_.reserve(execution_order_.size());
  for (size_t i = 0; i < execution_order_.size(); ++i) {
    completed_tile_counts_[i].store(0, std::memory_order_relaxed);
    node_precomputed_[i].store(false, std::memory_order_relaxed);
    output_mutexes_.push_back(std::make_unique<std::mutex>());
    tiled_node_preparation_states_.push_back(
        std::make_unique<TiledNodePreparationState>());
  }
  for (const PlannedTask& task : task_graph_.tasks) {
    if (task.kind != PlannedTaskKind::Tile) {
      continue;
    }
    auto idx_it = id_to_idx_.find(task.node_id);
    if (idx_it == id_to_idx_.end()) {
      continue;
    }
    const int node_idx = idx_it->second;
    ++tile_task_counts_[node_idx];
    planned_output_sizes_[node_idx] =
        merge_task_extent(planned_output_sizes_[node_idx], task.output_roi);
  }
}  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Executes one planned node and adds node context to recoverable errors.
 *
 * @param node_idx Dense index into the borrowed execution plan.
 * @return Nothing after the node's request-local result is available.
 * @throws std::bad_alloc when node execution exhausts memory.
 * @throws GraphError directly when pre-entry cancellation is observed, or
 * wrapping other standard and unknown failures, including provider exceptions
 * derived from std::exception.
 * @note Resource exhaustion retains its type for runtime/future transport;
 * cancellation occurs before node lookup and therefore intentionally has no
 * node-context wrapper. All later recoverable failures retain the existing
 * node-context diagnostic contract.
 */
void NodeTaskRunner::run_node(int node_idx) {
  observe_runner_cancellation(run_lease_);
  const int node_id = execution_order_.at(node_idx);
  try {
    compute_node(node_idx, node_id);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    std::rethrow_exception(compute_failure(graph_, node_id, e.what()));
  } catch (...) {
    std::rethrow_exception(
        compute_failure(graph_, node_id, "unknown exception"));
  }
}

/**
 * @brief Executes one planned task and adds task-node context to errors.
 *
 * @param task_id Dense id into the planned task graph.
 * @return Exact dependency-release ownership after task execution.
 * @throws std::bad_alloc when task execution or dependency access exhausts
 * memory.
 * @throws GraphError directly when pre-entry cancellation is observed, or
 * wrapping other range, standard, and unknown failures, including provider
 * exceptions derived from std::exception.
 * @note Cancellation occurs before task lookup and intentionally has no task
 * context. Tile tasks execute directly and observe every provider tile; node
 * and monolithic tasks delegate to run_node(), while execution transport
 * remains outside this runner.
 */
NodeTaskRunner::TaskDependencyRelease NodeTaskRunner::run_task(int task_id) {
  observe_runner_cancellation(run_lease_);
  const PlannedTask& task = task_graph_.tasks.at(task_id);
  try {
    if (task.kind == PlannedTaskKind::Tile) {
      return compute_tile_task(task);
    }
    auto idx_it = id_to_idx_.find(task.node_id);
    if (idx_it == id_to_idx_.end()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Task references unplanned node " + std::to_string(task.node_id));
    }
    run_node(idx_it->second);
    return TaskDependencyRelease::CurrentTask;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    std::rethrow_exception(compute_failure(graph_, task.node_id, e.what()));
  } catch (...) {
    std::rethrow_exception(
        compute_failure(graph_, task.node_id, "unknown exception"));
  }
}

bool NodeTaskRunner::allow_disk_cache() const {
  return !disable_disk_cache_ && !force_recache_;
}

/** @copydoc NodeTaskRunner::output_authority */
const PlannedOutputAuthority& NodeTaskRunner::output_authority(
    int node_idx) const {
  if (planned_work_ == nullptr) {
    throw GraphError(GraphErrc::ComputeError,
                     "Node execution has no frozen output-authority plan.");
  }
  const int node_id = execution_order_.at(node_idx);
  const auto found = std::find_if(planned_work_->begin(), planned_work_->end(),
                                  [node_id](const PlannedNodeWork& work) {
                                    return work.node_id == node_id;
                                  });
  if (found == planned_work_->end() || !found->output_authority.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Node execution is missing its frozen output authority.");
  }
  return *found->output_authority;
}

/** @copydoc NodeTaskRunner::upstream_output */
const NodeOutput* NodeTaskRunner::upstream_output(int up_id) const {
  if (up_id < 0) {
    return nullptr;
  }
  const Node* upstream = graph_.find_node(up_id);
  if (!upstream) {
    return nullptr;
  }
  auto it_idx = id_to_idx_.find(up_id);
  if (it_idx != id_to_idx_.end()) {
    const int up_idx = it_idx->second;
    if (temp_results_[up_idx].has_value()) {
      return &*temp_results_[up_idx];
    }
  }
  return ComputeCachePolicy::reusable_output(*upstream);
}

/** @copydoc NodeTaskRunner::has_reusable_memory_or_temp_output */
bool NodeTaskRunner::has_reusable_memory_or_temp_output(const Node& node,
                                                        int node_idx) const {
  return temp_results_[node_idx].has_value() ||
         ComputeCachePolicy::has_reusable_output(node);
}

/** @copydoc NodeTaskRunner::compute_node */
void NodeTaskRunner::compute_node(int node_idx, int node_id) {
  const Node& target_node = graph_.node(node_id);
  std::atomic_thread_fence(std::memory_order_acquire);

  if (!has_reusable_memory_or_temp_output(target_node, node_idx)) {
    try_load_disk_cache(target_node, node_idx);
  }
  if (!has_reusable_memory_or_temp_output(target_node, node_idx)) {
    compute_uncached_node(target_node, node_idx);
  }
}

/** @copydoc NodeTaskRunner::compute_tile_task */
NodeTaskRunner::TaskDependencyRelease NodeTaskRunner::compute_tile_task(
    const PlannedTask& task) {
  auto idx_it = id_to_idx_.find(task.node_id);
  if (idx_it == id_to_idx_.end()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Tile task references unplanned node " + std::to_string(task.node_id));
  }
  const int node_idx = idx_it->second;
  const Node& target_node = graph_.node(task.node_id);
  if (!force_recache_ && ComputeCachePolicy::has_reusable_output(target_node)) {
    node_precomputed_[node_idx].store(true, std::memory_order_release);
    return TaskDependencyRelease::CurrentTask;
  }
  if (node_precomputed_[node_idx].load(std::memory_order_acquire)) {
    return TaskDependencyRelease::CurrentTask;
  }

  const auto& op_opt = resolved_ops_[node_idx];
  if (!op_opt.has_value() || !op_opt->is_tiled()) {
    throw GraphError(
        GraphErrc::NoOperation,
        "No tiled op for " + target_node.type + ":" + target_node.subtype);
  }

  TiledNodeExecutionContext* execution_context =
      prepare_tiled_node_execution(node_idx, target_node, *op_opt);
  if (execution_context == nullptr) {
    return TaskDependencyRelease::CurrentTask;
  }
  HostOutputBinding* output_binding = tile_output_bindings_.at(node_idx).get();
  if (output_binding == nullptr) {
    throw GraphError(GraphErrc::ComputeError,
                     "Prepared tiled node has no Host output binding.");
  }
  TiledExecutionConfig tiled_config =
      tiled_config_for(target_node, execution_context->implementation);
  tiled_config.tile_size =
      task.tile_size > 0 ? task.tile_size : tiled_config.tile_size;
  tiled_config.output_roi = task.output_roi;
  tiled_config.output_size = planned_output_sizes_.at(node_idx);

  BenchmarkEvent current_event = start_event(target_node);
  NodeExecutor::execute_tiled_context_into_binding(
      graph_, execution_context->node_for_exec,
      std::get<TileOpFunc>(execution_context->implementation.func),
      execution_context->input_context, *output_binding, tiled_config);
  return finalize_tiled_node_if_complete(
             node_idx, target_node, execution_context->input_context.inputs,
             current_event)
             ? TaskDependencyRelease::CompleteTiledNode
             : TaskDependencyRelease::DeferTiledNode;
}

/** @copydoc NodeTaskRunner::prepare_tiled_node_execution */
NodeTaskRunner::TiledNodeExecutionContext*
NodeTaskRunner::prepare_tiled_node_execution(
    int node_idx, const Node& target_node,
    const OpImplementation& implementation) {
  TiledNodePreparationState& state =
      *tiled_node_preparation_states_.at(node_idx);
  std::lock_guard<std::mutex> preparation_lock(state.preparation_mutex);
  if (node_precomputed_[node_idx].load(std::memory_order_acquire)) {
    return nullptr;
  }
  if (state.execution_context) {
    if (state.execution_context->implementation.implementation_identity !=
        implementation.implementation_identity) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Tiled node preparation disagrees with selected implementation.");
    }
    return state.execution_context.get();
  }
  if (state.initialization_failure) {
    std::rethrow_exception(state.initialization_failure);
  }
  if (state.initialization_attempted) {
    throw GraphError(GraphErrc::ComputeError,
                     "Tiled node preparation has incomplete state.");
  }
  state.initialization_attempted = true;
  try {
    if (try_satisfy_tile_from_disk_cache(target_node, node_idx)) {
      return nullptr;
    }
    auto execution_context = std::make_unique<TiledNodeExecutionContext>(
        TiledNodeExecutionContext{target_node, implementation, {}});
    execution_context->node_for_exec.runtime_parameters =
        resolve_runtime_parameters(target_node);
    const std::vector<const NodeOutput*> image_inputs =
        resolve_image_inputs(target_node);
    execution_context->input_context =
        NodeExecutor::prepare_tiled_input_context(
            execution_context->node_for_exec, image_inputs);
    HostOutputBinding* output_binding =
        ensure_tile_output_binding(node_idx, execution_context->node_for_exec,
                                   execution_context->implementation,
                                   execution_context->input_context.inputs);
    if (output_binding == nullptr) {
      return nullptr;
    }
    state.execution_context = std::move(execution_context);
    return state.execution_context.get();
  } catch (...) {
    state.initialization_failure = std::current_exception();
    throw;
  }
}

/** @copydoc NodeTaskRunner::try_satisfy_tile_from_disk_cache */
bool NodeTaskRunner::try_satisfy_tile_from_disk_cache(const Node& target_node,
                                                      int node_idx) {
  if (!allow_disk_cache()) {
    return node_precomputed_[node_idx].load(std::memory_order_acquire);
  }

  std::lock_guard<std::mutex> lock(*output_mutexes_.at(node_idx));
  if (node_precomputed_[node_idx].load(std::memory_order_acquire)) {
    return true;
  }
  if (tile_output_bindings_[node_idx] || temp_results_[node_idx].has_value()) {
    return false;
  }

  try_load_disk_cache(target_node, node_idx);
  if (temp_results_[node_idx].has_value()) {
    node_precomputed_[node_idx].store(true, std::memory_order_release);
    return true;
  }
  return false;
}

/** @copydoc NodeTaskRunner::ensure_tile_output_binding */
HostOutputBinding* NodeTaskRunner::ensure_tile_output_binding(
    int node_idx, const Node& target_node,
    const OpImplementation& implementation,
    const std::vector<const NodeOutput*>& image_inputs) {
  std::lock_guard<std::mutex> lock(*output_mutexes_.at(node_idx));
  if (node_precomputed_[node_idx].load(std::memory_order_acquire)) {
    return nullptr;
  }
  if (!tile_output_bindings_[node_idx]) {
    const PixelSize planned_size = planned_output_sizes_.at(node_idx);
    const PixelSize output_size =
        planned_size.width > 0 && planned_size.height > 0
            ? planned_size
            : PixelSize{
                  as_int_flexible(target_node.runtime_parameters, "width", 256),
                  as_int_flexible(target_node.runtime_parameters, "height",
                                  256)};
    tile_output_bindings_[node_idx] = std::make_unique<HostOutputBinding>(
        NodeExecutor::allocate_tiled_output_binding(
            target_node, image_inputs, output_size,
            implementation.tiled_output_inference));
  }
  return tile_output_bindings_[node_idx].get();
}

/** @copydoc NodeTaskRunner::finalize_tiled_node_if_complete */
bool NodeTaskRunner::finalize_tiled_node_if_complete(
    int node_idx, const Node& target_node,
    const std::vector<const NodeOutput*>& image_inputs,
    BenchmarkEvent& current_event) {
  const int expected_tiles = tile_task_counts_.at(node_idx);
  if (expected_tiles <= 0) {
    return false;
  }
  const int previous =
      completed_tile_counts_[node_idx].fetch_add(1, std::memory_order_acq_rel);
  if (previous + 1 != expected_tiles) {
    return false;
  }
  std::unique_ptr<HostOutputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(*output_mutexes_.at(node_idx));
    binding = std::move(tile_output_bindings_[node_idx]);
  }
  if (!binding) {
    throw GraphError(GraphErrc::ComputeError,
                     "Completed tiled node has no Host output binding.");
  }
  NodeOutput output;
  output.publish_image_value(binding->seal());
  const double execution_ms =
      record_computed_output(target_node, current_event);
  finalize_output_metadata(output, image_inputs, enable_timing_, execution_ms);
  validate_planned_output(output, output_authority(node_idx),
                          PlannedOutputReadiness::RequireReady);
  temp_results_[node_idx] = std::move(output);
  return true;
}

void NodeTaskRunner::try_load_disk_cache(const Node& target_node,
                                         int node_idx) {
  if (!allow_disk_cache()) {
    return;
  }
  const PlannedOutputAuthority& authority = output_authority(node_idx);
  const ValueDiskCacheOutputSchema disk_schema{
      authority.image_output_name.has_value(), authority.parameter_output_names,
      authority.named_value_output_names};
  NodeOutput from_disk;
  if (cache_.try_load_from_disk_cache_into(graph_, target_node, from_disk,
                                           disk_schema)) {
    validate_planned_output(from_disk, authority,
                            PlannedOutputReadiness::RequireReady);
    temp_results_[node_idx] = std::move(from_disk);
    record_disk_cache_hit(target_node);
  }
}

/**
 * @brief Builds execution parameters from static and same-request outputs.
 * @param target_node Node whose effective parameters are resolved.
 * @return Deep ParameterMap copy with connected parameter values overlaid.
 * @throws GraphError when a connected parameter output is unavailable.
 * @throws std::bad_alloc from recursive value copying.
 * @note The result is request-local and does not mutate committed node state.
 */
plugin::ParameterMap NodeTaskRunner::resolve_runtime_parameters(
    const Node& target_node) const {
  plugin::ParameterMap runtime_params = target_node.parameters;
  for (const auto& p_input : target_node.parameter_inputs) {
    if (p_input.from_node_id < 0) {
      continue;
    }
    auto const* up_out = upstream_output(p_input.from_node_id);
    if (!up_out) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Parameter input not ready for node " +
                           std::to_string(target_node.id));
    }
    auto it = up_out->data.find(p_input.from_output_name);
    if (it == up_out->data.end()) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(p_input.from_node_id) +
                           " missing output '" + p_input.from_output_name +
                           "'");
    }
    runtime_params.insert_or_assign(p_input.to_parameter_name, it->second);
  }
  return runtime_params;
}

/**
 * @brief Resolves image outputs while preserving destination input indexes.
 * @param target_node Node whose declared image slots are resolved.
 * @return Vector aligned exactly with target_node.image_inputs; disconnected
 * slots contain nullptr.
 * @throws GraphError when a connected source output is not ready.
 * @throws std::bad_alloc when vector allocation fails.
 * @note Preserving null slots lets executor ROI snapshots retain graph edge
 * indexes when, for example, slot zero is disconnected and slot one is live.
 */
std::vector<const NodeOutput*> NodeTaskRunner::resolve_image_inputs(
    const Node& target_node) const {
  std::vector<const NodeOutput*> inputs_ready(target_node.image_inputs.size(),
                                              nullptr);
  for (std::size_t index = 0; index < target_node.image_inputs.size();
       ++index) {
    const ImageInput& i_input = target_node.image_inputs[index];
    if (i_input.from_node_id < 0) {
      continue;
    }
    auto const* up_out = upstream_output(i_input.from_node_id);
    if (!up_out) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "Image input not ready for node " + std::to_string(target_node.id));
    }
    inputs_ready[index] = up_out;
  }
  return inputs_ready;
}

/** @copydoc NodeTaskRunner::tiled_config_for */
TiledExecutionConfig NodeTaskRunner::tiled_config_for(
    const Node& target_node, const OpImplementation& implementation) const {
  TiledExecutionConfig tiled_config;
  if (!implementation.is_tiled()) {
    return tiled_config;
  }
  tiled_config.metadata = implementation.metadata;
  tiled_config.dirty_propagator = implementation.dirty_propagator;
  tiled_config.tiled_output_inference = implementation.tiled_output_inference;
  tiled_config.implementation_identity = implementation.implementation_identity;
  if (implementation.metadata.tile_preference == TileSizePreference::MICRO) {
    tiled_config.tile_size = 16;
  } else if (implementation.metadata.tile_preference ==
             TileSizePreference::MACRO) {
    tiled_config.tile_size = 256;
  }
  tiled_config.on_tile = [this, node_id = target_node.id](const PixelRect&) {
    observe_runner_cancellation(run_lease_);
    task_runtime_.log_event(ExecutionTraceAction::ExecuteTile, node_id);
  };
  return tiled_config;
}

BenchmarkEvent NodeTaskRunner::start_event(const Node& target_node) const {
  BenchmarkEvent event;
  event.node_id = target_node.id;
  event.op_name = make_key(target_node.type, target_node.subtype);
  event.dependency_start_time = std::chrono::high_resolution_clock::now();
  event.execution_start_time = event.dependency_start_time;
  return event;
}

void NodeTaskRunner::compute_uncached_node(const Node& target_node,
                                           int node_idx) {
  const auto& op_opt = resolved_ops_[node_idx];
  if (!op_opt.has_value()) {
    throw GraphError(GraphErrc::NoOperation, "No op for " + target_node.type +
                                                 ":" + target_node.subtype);
  }

  plugin::ParameterMap runtime_params = resolve_runtime_parameters(target_node);
  std::vector<const NodeOutput*> inputs_ready =
      resolve_image_inputs(target_node);
  BenchmarkEvent current_event = start_event(target_node);

  Node node_for_exec = target_node;
  node_for_exec.runtime_parameters = runtime_params;
  TiledExecutionConfig tiled_config = tiled_config_for(target_node, *op_opt);
  NodeOutput result = NodeExecutor::execute(graph_, node_for_exec, op_opt->func,
                                            inputs_ready, tiled_config);

  const double execution_ms =
      record_computed_output(target_node, current_event);
  finalize_output_metadata(result, inputs_ready, enable_timing_, execution_ms);
  validate_planned_output(result, output_authority(node_idx),
                          PlannedOutputReadiness::AllowPending);
  temp_results_[node_idx] = std::move(result);
}

void NodeTaskRunner::record_disk_cache_hit(const Node& target_node) {
  if (!enable_timing_) {
    events_.push(target_node.id, target_node.name, "disk_cache", 0.0);
    return;
  }

  BenchmarkEvent event = start_event(target_node);
  event.execution_end_time = event.execution_start_time;
  event.execution_duration_ms = 0.0;
  event.source = "disk_cache";
  if (benchmark_events_) {
    std::lock_guard lk(timing_mutex_);
    benchmark_events_->push_back(event);
  }
  {
    std::lock_guard lk(timing_mutex_);
    timing_results_.node_timings.push_back(
        {target_node.id, target_node.name, 0.0, "disk_cache"});
  }
  events_.push(target_node.id, target_node.name, "disk_cache", 0.0);
}

double NodeTaskRunner::record_computed_output(const Node& target_node,
                                              BenchmarkEvent& current_event) {
  if (!enable_timing_) {
    events_.push(target_node.id, target_node.name, "computed", 0.0);
    return 0.0;
  }

  current_event.execution_end_time = std::chrono::high_resolution_clock::now();
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          current_event.execution_end_time - current_event.execution_start_time)
          .count();
  current_event.source = "computed";
  current_event.execution_duration_ms = elapsed_ms;
  if (benchmark_events_) {
    std::lock_guard lk(timing_mutex_);
    benchmark_events_->push_back(current_event);
  }
  {
    std::lock_guard lk(timing_mutex_);
    timing_results_.node_timings.push_back(
        {target_node.id, target_node.name, elapsed_ms, "computed"});
  }
  events_.push(target_node.id, target_node.name, "computed", elapsed_ms);
  return elapsed_ms;
}

}  // namespace ps::compute
