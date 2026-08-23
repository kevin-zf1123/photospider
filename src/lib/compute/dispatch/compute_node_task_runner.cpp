#include "compute/dispatch/compute_node_task_runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include "compute/execution/execution_service.hpp"
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

/**
 * @brief Reports whether one node declares any live parameter connection.
 * @param node Immutable node whose parameter edges are inspected.
 * @return True when at least one parameter source has a nonnegative node id.
 * @throws Nothing.
 * @note Disconnected declarations remain explicit topology but never require a
 * destination key, a source value, or a supplemental reservation slot.
 */
bool has_connected_parameter_inputs(const Node& node) noexcept {
  return std::any_of(
      node.parameter_inputs.begin(), node.parameter_inputs.end(),
      [](const ParameterInput& input) { return input.from_node_id >= 0; });
}

/**
 * @brief Compares recursive values at the retained-snapshot validation edge.
 * @param admitted Deep-owned value frozen before retained admission.
 * @param current Current static value or value still published by the exact
 * captured connected source owner.
 * @return True only when alternatives, array order, object keys, and recursive
 * content match, except that paired double NaN leaves compare reflexively.
 * @throws Nothing; inspection is read-only and performs no allocation.
 * @note This private equivalence does not change public
 * `ParameterValue::operator==`, which retains ordinary IEEE NaN behavior.
 * Exact connected source identity is validated separately before this helper.
 * Inputs must remain alive and immutable for the call; recursion uses only the
 * caller thread's stack and acquires no lock or lifetime ownership.
 */
bool retained_parameter_snapshot_equivalent(
    const plugin::ParameterValue& admitted,
    const plugin::ParameterValue& current) noexcept {
  if (admitted.kind() != current.kind()) {
    return false;
  }
  switch (admitted.kind()) {
    case plugin::ParameterKind::Null:
      return true;
    case plugin::ParameterKind::Bool: {
      const auto* admitted_value = std::get_if<bool>(&admitted.storage());
      const auto* current_value = std::get_if<bool>(&current.storage());
      return admitted_value != nullptr && current_value != nullptr &&
             *admitted_value == *current_value;
    }
    case plugin::ParameterKind::Int64: {
      const auto* admitted_value =
          std::get_if<std::int64_t>(&admitted.storage());
      const auto* current_value = std::get_if<std::int64_t>(&current.storage());
      return admitted_value != nullptr && current_value != nullptr &&
             *admitted_value == *current_value;
    }
    case plugin::ParameterKind::Double: {
      const auto* admitted_value = std::get_if<double>(&admitted.storage());
      const auto* current_value = std::get_if<double>(&current.storage());
      return admitted_value != nullptr && current_value != nullptr &&
             (*admitted_value == *current_value ||
              (std::isnan(*admitted_value) && std::isnan(*current_value)));
    }
    case plugin::ParameterKind::String: {
      const auto* admitted_value =
          std::get_if<std::string>(&admitted.storage());
      const auto* current_value = std::get_if<std::string>(&current.storage());
      return admitted_value != nullptr && current_value != nullptr &&
             *admitted_value == *current_value;
    }
    case plugin::ParameterKind::Array: {
      const auto* admitted_value =
          std::get_if<plugin::ParameterValue::Array>(&admitted.storage());
      const auto* current_value =
          std::get_if<plugin::ParameterValue::Array>(&current.storage());
      if (admitted_value == nullptr || current_value == nullptr ||
          admitted_value->size() != current_value->size()) {
        return false;
      }
      for (std::size_t index = 0U; index < admitted_value->size(); ++index) {
        if (!retained_parameter_snapshot_equivalent((*admitted_value)[index],
                                                    (*current_value)[index])) {
          return false;
        }
      }
      return true;
    }
    case plugin::ParameterKind::Object: {
      const auto* admitted_value =
          std::get_if<plugin::ParameterValue::Object>(&admitted.storage());
      const auto* current_value =
          std::get_if<plugin::ParameterValue::Object>(&current.storage());
      if (admitted_value == nullptr || current_value == nullptr ||
          admitted_value->size() != current_value->size()) {
        return false;
      }
      auto admitted_entry = admitted_value->begin();
      auto current_entry = current_value->begin();
      for (; admitted_entry != admitted_value->end();
           ++admitted_entry, ++current_entry) {
        if (admitted_entry->first != current_entry->first ||
            !retained_parameter_snapshot_equivalent(admitted_entry->second,
                                                    current_entry->second)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
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
    if (state->execution_context) {
      estimate.add_objects<TiledNodeExecutionContext>();
      estimate.add_bytes(node_dynamic_retained_memory_bytes(
          state->execution_context->node_for_exec));
      estimate.add_objects<const plugin::ParameterValue*>(
          static_cast<std::uint64_t>(
              state->execution_context->connected_parameter_sources
                  .capacity()));
      estimate.add_objects<const NodeOutput*>(static_cast<std::uint64_t>(
          state->execution_context->input_context.inputs().capacity()));
      estimate.add_objects<NodeOutput>(static_cast<std::uint64_t>(
          state->execution_context->input_context.normalized_storage()
              .capacity()));
      for (const NodeOutput& output :
           state->execution_context->input_context.normalized_storage()) {
        estimate.add_bytes(node_output_dynamic_retained_memory_bytes(output));
      }
    }
  }
  return estimate.bytes();
}

/**
 * @copydoc NodeTaskRunner::tiled_context_borrows_resolved_operation_for_testing
 */
bool NodeTaskRunner::tiled_context_borrows_resolved_operation_for_testing(
    std::size_t node_idx) const noexcept {
  if (node_idx >= tiled_node_preparation_states_.size() ||
      node_idx >= resolved_ops_.size()) {
    return false;
  }
  const std::unique_ptr<TiledNodePreparationState>& state =
      tiled_node_preparation_states_[node_idx];
  const std::optional<OpImplementation>& resolved = resolved_ops_[node_idx];
  return state && state->execution_context_prepared &&
         state->execution_context && resolved.has_value() &&
         state->execution_context->implementation == &*resolved;
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
      planned_work_(context.planned_work),
      execution_service_(
          dynamic_cast<ExecutionService*>(&context.task_runtime)) {
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
  for (std::size_t index = 0U; index < execution_order_.size(); ++index) {
    if (tile_task_counts_.at(index) <= 0) {
      continue;
    }
    const Node& target_node = graph_.node(execution_order_.at(index));
    Node node_for_exec = target_node;
    node_for_exec.runtime_parameters = node_for_exec.parameters;
    for (const ParameterInput& input : node_for_exec.parameter_inputs) {
      if (input.from_node_id < 0) {
        continue;
      }
      if (node_for_exec.runtime_parameters.find(input.to_parameter_name) ==
          node_for_exec.runtime_parameters.end()) {
        node_for_exec.runtime_parameters.emplace(input.to_parameter_name,
                                                 plugin::ParameterValue{});
      }
    }
    const std::optional<OpImplementation>& resolved = resolved_ops_.at(index);
    TiledNodePreparationState& state =
        *tiled_node_preparation_states_.at(index);
    state.execution_context =
        std::make_unique<TiledNodeExecutionContext>(TiledNodeExecutionContext{
            std::move(node_for_exec),
            resolved.has_value() ? &*resolved : nullptr,
            TiledInputNormalizer::preallocate(target_node.image_inputs.size()),
            {},
            false,
            false});
    const std::size_t connected_parameter_count = static_cast<std::size_t>(
        std::count_if(target_node.parameter_inputs.begin(),
                      target_node.parameter_inputs.end(),
                      [](const ParameterInput& input) {
                        return input.from_node_id >= 0;
                      }));
    state.execution_context->connected_parameter_sources.reserve(
        connected_parameter_count);
    if (has_connected_parameter_inputs(target_node)) {
      if (!try_materialize_runtime_parameters_before_admission(
              target_node, state.execution_context.get())) {
        ++supplemental_retained_reservation_count_;
      }
    } else {
      state.execution_context->runtime_parameters_materialized = true;
    }
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
    if (force_recache_) {
      return nullptr;
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
      tiled_config_for(target_node, *execution_context->implementation);
  tiled_config.tile_size =
      task.tile_size > 0 ? task.tile_size : tiled_config.tile_size;
  tiled_config.output_roi = task.output_roi;
  tiled_config.output_size = planned_output_sizes_.at(node_idx);

  BenchmarkEvent current_event = start_event(target_node);
  NodeExecutor::execute_tiled_context_into_binding(
      graph_, execution_context->node_for_exec,
      std::get<TileOpFunc>(execution_context->implementation->func),
      execution_context->input_context, *output_binding, tiled_config);
  return finalize_tiled_node_if_complete(
             node_idx, target_node, execution_context->input_context.inputs(),
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
  if (state.execution_context_prepared) {
    if (!state.execution_context ||
        state.execution_context->implementation != &implementation) {
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
    if (!state.execution_context ||
        state.execution_context->implementation != &implementation) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Tiled node preparation lost its admitted execution owner.");
    }
    TiledNodeExecutionContext& execution_context = *state.execution_context;
    materialize_or_validate_runtime_parameters(target_node, &execution_context);
    const std::vector<const NodeOutput*> image_inputs =
        resolve_image_inputs(target_node);
    execution_context.input_context = NodeExecutor::prepare_tiled_input_context(
        execution_context.node_for_exec, image_inputs,
        std::move(execution_context.input_context));
    HostOutputBinding* output_binding =
        ensure_tile_output_binding(node_idx, execution_context.node_for_exec,
                                   *execution_context.implementation,
                                   execution_context.input_context.inputs());
    if (output_binding == nullptr) {
      return nullptr;
    }
    state.execution_context_prepared = true;
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
 * @param captured_sources Optional destination cleared and filled with the
 * exact borrowed address of every connected source value in declaration order.
 * @return Deep ParameterMap copy with connected parameter values overlaid.
 * @throws GraphError when a connected parameter output is unavailable.
 * @throws std::logic_error, std::invalid_argument, or std::overflow_error when
 * reusable formal-output validity cannot be checked.
 * @throws std::bad_alloc from recursive value copying, map growth, formal
 * validity checking, or captured-source vector growth.
 * @note The result is request-local and does not mutate committed node state.
 * Captured pointers freeze source-object identity rather than value equality
 * and borrow graph or same-Run temporary output lifetime. Product callers
 * pre-reserve the complete vector capacity before Run admission.
 */
plugin::ParameterMap NodeTaskRunner::resolve_runtime_parameters(
    const Node& target_node,
    std::vector<const plugin::ParameterValue*>* captured_sources) const {
  if (captured_sources != nullptr) {
    captured_sources->clear();
  }
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
    if (captured_sources != nullptr) {
      captured_sources->push_back(&it->second);
    }
    runtime_params.insert_or_assign(p_input.to_parameter_name, it->second);
  }
  return runtime_params;
}

/**
 * @brief Validates an admitted effective map and captured source identities.
 * @param target_node Current graph node providing static values and edges.
 * @param execution_context Frozen map and exact source addresses to validate.
 * @return Nothing when map shape, snapshot-equivalent recursive values, and
 * source identities still match.
 * @throws GraphError when a dependency/output is missing, the map changed, or
 * an equal-content source owner replaced the admitted identity.
 * @throws std::logic_error, std::invalid_argument, or std::overflow_error when
 * reusable formal-output validity cannot be checked.
 * @throws std::bad_alloc when formal validity checking or failure-diagnostic
 * construction cannot allocate.
 * @note Connected declarations apply in order and the last repeated
 * destination wins. The equivalence traversal allocates nothing, and
 * successful validation copies no payload or mutates the admitted map; formal
 * validity checks and failure diagnostics may allocate. Paired double NaN
 * leaves compare reflexively only at this validation boundary; all other
 * alternatives and recursive structure remain exact. Address comparison
 * deliberately keeps equal-content replacement fail closed.
 */
void NodeTaskRunner::validate_materialized_runtime_parameters(
    const Node& target_node,
    const TiledNodeExecutionContext& execution_context) const {
  const plugin::ParameterMap& admitted_parameters =
      execution_context.node_for_exec.runtime_parameters;
  std::size_t expected_parameter_count = target_node.parameters.size();
  std::size_t connected_source_index = 0U;
  for (std::size_t input_index = 0U;
       input_index < target_node.parameter_inputs.size(); ++input_index) {
    const ParameterInput& input = target_node.parameter_inputs.at(input_index);
    if (input.from_node_id < 0) {
      continue;
    }
    bool destination_already_counted =
        target_node.parameters.find(input.to_parameter_name) !=
        target_node.parameters.end();
    for (std::size_t prior_index = 0U;
         !destination_already_counted && prior_index < input_index;
         ++prior_index) {
      const ParameterInput& prior =
          target_node.parameter_inputs.at(prior_index);
      destination_already_counted =
          prior.from_node_id >= 0 &&
          prior.to_parameter_name == input.to_parameter_name;
    }
    if (!destination_already_counted) {
      if (expected_parameter_count == std::numeric_limits<std::size_t>::max()) {
        throw GraphError(GraphErrc::ComputeError,
                         "Runtime parameter count overflow.");
      }
      ++expected_parameter_count;
    }

    const NodeOutput* upstream = upstream_output(input.from_node_id);
    if (upstream == nullptr) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Parameter input not ready for node " +
                           std::to_string(target_node.id));
    }
    const auto source = upstream->data.find(input.from_output_name);
    if (source == upstream->data.end()) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(input.from_node_id) +
                           " missing output '" + input.from_output_name + "'");
    }
    if (connected_source_index >=
            execution_context.connected_parameter_sources.size() ||
        execution_context.connected_parameter_sources.at(
            connected_source_index) != &source->second) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Connected runtime parameter source identity changed after retained "
          "admission.");
    }
    ++connected_source_index;

    bool later_destination = false;
    for (std::size_t later_index = input_index + 1U;
         later_index < target_node.parameter_inputs.size(); ++later_index) {
      const ParameterInput& later =
          target_node.parameter_inputs.at(later_index);
      if (later.from_node_id >= 0 &&
          later.to_parameter_name == input.to_parameter_name) {
        later_destination = true;
        break;
      }
    }
    if (!later_destination) {
      const auto destination =
          admitted_parameters.find(input.to_parameter_name);
      if (destination == admitted_parameters.end() ||
          !retained_parameter_snapshot_equivalent(destination->second,
                                                  source->second)) {
        throw GraphError(
            GraphErrc::ComputeError,
            "Connected runtime parameters changed after retained admission.");
      }
    }
  }
  if (connected_source_index !=
      execution_context.connected_parameter_sources.size()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Connected runtime parameter source set changed after retained "
        "admission.");
  }
  for (const auto& static_parameter : target_node.parameters) {
    const bool connected_destination =
        std::any_of(target_node.parameter_inputs.begin(),
                    target_node.parameter_inputs.end(),
                    [&static_parameter](const ParameterInput& input) {
                      return input.from_node_id >= 0 &&
                             input.to_parameter_name == static_parameter.first;
                    });
    if (connected_destination) {
      continue;
    }
    const auto admitted = admitted_parameters.find(static_parameter.first);
    if (admitted == admitted_parameters.end() ||
        !retained_parameter_snapshot_equivalent(admitted->second,
                                                static_parameter.second)) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Static runtime parameters changed after retained admission.");
    }
  }
  if (admitted_parameters.size() != expected_parameter_count) {
    throw GraphError(GraphErrc::ComputeError,
                     "Runtime parameter map shape changed after retained "
                     "admission.");
  }
}

/** @copydoc NodeTaskRunner::try_materialize_runtime_parameters_before_admission
 */
bool NodeTaskRunner::try_materialize_runtime_parameters_before_admission(
    const Node& target_node,
    TiledNodeExecutionContext* execution_context) const {
  if (execution_context == nullptr) {
    throw std::invalid_argument(
        "Tiled runtime-parameter materialization requires an owned context.");
  }
  try {
    plugin::ParameterMap materialized = resolve_runtime_parameters(
        target_node, &execution_context->connected_parameter_sources);
    execution_context->node_for_exec.runtime_parameters.swap(materialized);
    execution_context->runtime_parameters_materialized = true;
    return true;
  } catch (const GraphError& error) {
    if (error.code() == GraphErrc::MissingDependency) {
      execution_context->connected_parameter_sources.clear();
      return false;
    }
    throw;
  }
}

/**
 * @brief Validates an early map or admits and installs one late connected map.
 * @param target_node Immutable graph node whose ready sources are resolved.
 * @param execution_context Non-null stable tiled context owned by this runner.
 * @return Nothing after exact validation, optional service supplemental
 * admission, and a no-throw map swap.
 * @throws std::invalid_argument when execution_context is null, retained formal
 * image/tensor facts violate their declared contracts, or a service-backed
 * supplemental call crosses an invalid service/worker/Run boundary.
 * @throws std::logic_error when reusable formal-output validity cannot be
 * checked through a valid Value accessor, a service-backed supplemental path
 * lacks its Run lease or current worker Run, service shutdown prevents
 * admission, or no pre-accounted owner slot remains.
 * @throws std::overflow_error when a retained formal logical extent exceeds
 * Region bounds.
 * @throws GraphError when a dependency/value/source identity changed, retained
 * arithmetic overflowed, cancellation/failure won, or service policy/ledger
 * admission rejects the delta.
 * @throws std::bad_alloc from recursive candidate materialization,
 * connected-source or diagnostic storage, formal validity checking, or
 * service supplemental reservation preparation.
 * @throws std::system_error from service supplemental reservation preparation
 * or synchronization.
 * @note The candidate remains unpublished during fallible preparation.
 * Recursive resolution and formal validity/diagnostic work may allocate. A
 * service-backed route compares actual recursive map bytes with the already
 * charged placeholder owner and admits only the positive delta through a
 * distinct retained-only root. A generic `ExecutionTaskRuntime` has no
 * `ExecutionService` Run-ledger authority, so its completed candidate instead
 * transfers directly into the runner-owned map. In both routes, statically
 * no-throw `clear()` and `swap()` release placeholder nodes before installing
 * the candidate, so two retained destination owners never coexist.
 * Disconnected declarations remain inert.
 */
void NodeTaskRunner::materialize_or_validate_runtime_parameters(
    const Node& target_node,
    TiledNodeExecutionContext* execution_context) const {
  if (execution_context == nullptr) {
    throw std::invalid_argument(
        "Tiled runtime-parameter preparation requires an owned context.");
  }
  if (execution_context->runtime_parameters_materialized) {
    validate_materialized_runtime_parameters(target_node, *execution_context);
    return;
  }
  plugin::ParameterMap materialized = resolve_runtime_parameters(
      target_node, &execution_context->connected_parameter_sources);
  plugin::ParameterMap& admitted_parameters =
      execution_context->node_for_exec.runtime_parameters;

  const std::uint64_t admitted_bytes =
      parameter_map_dynamic_retained_memory_bytes(admitted_parameters);
  const std::uint64_t materialized_bytes =
      parameter_map_dynamic_retained_memory_bytes(materialized);
  const std::uint64_t supplemental_bytes =
      materialized_bytes > admitted_bytes ? materialized_bytes - admitted_bytes
                                          : 0U;
  if (supplemental_bytes != 0U && execution_service_ != nullptr) {
    if (run_lease_ == nullptr) {
      throw std::logic_error(
          "Deferred connected-parameter admission requires a Run lease.");
    }
    execution_service_->retain_current_run_shared_reservation(
        *run_lease_, supplemental_bytes);
    execution_context->runtime_parameters_supplementally_admitted = true;
  }
  static_assert(noexcept(std::declval<plugin::ParameterMap&>().swap(
                    std::declval<plugin::ParameterMap&>())),
                "Connected parameter installation requires no-throw map swap");
  static_assert(noexcept(std::declval<plugin::ParameterMap&>().clear()),
                "Connected parameter replacement requires no-throw map clear");
  admitted_parameters.clear();
  admitted_parameters.swap(materialized);
  execution_context->runtime_parameters_materialized = true;
}

/** @copydoc NodeTaskRunner::release_supplemental_runtime_parameters */
void NodeTaskRunner::release_supplemental_runtime_parameters() noexcept {
  try {
    for (const std::unique_ptr<TiledNodePreparationState>& state :
         tiled_node_preparation_states_) {
      if (!state) {
        continue;
      }
      std::lock_guard<std::mutex> preparation_lock(state->preparation_mutex);
      if (!state->execution_context ||
          !state->execution_context
               ->runtime_parameters_supplementally_admitted) {
        continue;
      }
      state->execution_context->node_for_exec.runtime_parameters.clear();
      state->execution_context->connected_parameter_sources.clear();
      state->execution_context->runtime_parameters_materialized = false;
      state->execution_context->runtime_parameters_supplementally_admitted =
          false;
    }
  } catch (...) {
    std::terminate();
  }
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
