#include "compute/compute_node_task_runner.hpp"

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

#include "compute/compute_metrics_recorder.hpp"
#include "compute/node_executor.hpp"
#include "compute/resource_demand_estimator.hpp"
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
 * @note Worker tasks use this helper before rethrowing so scheduler exception
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
 * @brief Infers channel count and data type for a tile output buffer.
 *
 * @param image_inputs Ready image inputs for the node.
 * @return Channel count and DataType from the first connected input, or the
 * generator default when every declared slot is disconnected.
 * @throws Nothing.
 * @note Null entries preserve graph input-slot identity and are skipped only
 * for format inference.
 */
std::pair<int, DataType> infer_tile_channels_and_type(
    const std::vector<const NodeOutput*>& image_inputs) {
  for (const NodeOutput* input : image_inputs) {
    if (input) {
      return {input->image_buffer.channels, input->image_buffer.type};
    }
  }
  return {1, DataType::FLOAT32};
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
  for (const std::unique_ptr<std::mutex>& mutex : output_mutexes_) {
    if (mutex) {
      estimate.add_objects<std::mutex>();
    }
  }
  return estimate.bytes();
}

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
      benchmark_events_(context.benchmark_events) {
  planned_output_sizes_.assign(execution_order_.size(), PixelSize{});
  tile_task_counts_.assign(execution_order_.size(), 0);
  completed_tile_counts_ =
      std::vector<std::atomic<int>>(execution_order_.size());
  node_precomputed_ = std::vector<std::atomic<bool>>(execution_order_.size());
  output_mutexes_.reserve(execution_order_.size());
  for (size_t i = 0; i < execution_order_.size(); ++i) {
    completed_tile_counts_[i].store(0, std::memory_order_relaxed);
    node_precomputed_[i].store(false, std::memory_order_relaxed);
    output_mutexes_.push_back(std::make_unique<std::mutex>());
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
 * @return Nothing.
 * @throws std::bad_alloc when node execution exhausts memory.
 * @throws GraphError wrapping other standard and unknown failures, including
 * provider exceptions derived from std::exception.
 * @note Resource exhaustion retains its type for scheduler/future transport;
 * all other failures retain the existing node-context diagnostic contract.
 */
void NodeTaskRunner::run_node(int node_idx) {
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
 * @return Nothing.
 * @throws std::bad_alloc when task execution or dependency access exhausts
 * memory.
 * @throws GraphError wrapping other range, standard, and unknown failures,
 * including provider exceptions derived from std::exception.
 * @note Tile tasks execute directly; node and monolithic tasks delegate to
 * run_node(), while scheduler transport remains outside this runner.
 */
void NodeTaskRunner::run_task(int task_id) {
  const PlannedTask& task = task_graph_.tasks.at(task_id);
  try {
    if (task.kind == PlannedTaskKind::Tile) {
      compute_tile_task(task);
      return;
    }
    auto idx_it = id_to_idx_.find(task.node_id);
    if (idx_it == id_to_idx_.end()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Task references unplanned node " + std::to_string(task.node_id));
    }
    run_node(idx_it->second);
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
  if (upstream->cached_output_high_precision.has_value()) {
    return &*upstream->cached_output_high_precision;
  }
  return nullptr;
}

bool NodeTaskRunner::has_memory_or_temp_output(const Node& node,
                                               int node_idx) const {
  return node.cached_output_high_precision.has_value() ||
         temp_results_[node_idx].has_value();
}

void NodeTaskRunner::compute_node(int node_idx, int node_id) {
  const Node& target_node = graph_.node(node_id);
  std::atomic_thread_fence(std::memory_order_acquire);

  if (!has_memory_or_temp_output(target_node, node_idx)) {
    try_load_disk_cache(target_node, node_idx);
  }
  if (!has_memory_or_temp_output(target_node, node_idx)) {
    compute_uncached_node(target_node, node_idx);
  }
}

void NodeTaskRunner::compute_tile_task(const PlannedTask& task) {
  auto idx_it = id_to_idx_.find(task.node_id);
  if (idx_it == id_to_idx_.end()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Tile task references unplanned node " + std::to_string(task.node_id));
  }
  const int node_idx = idx_it->second;
  const Node& target_node = graph_.node(task.node_id);
  if (!force_recache_ && target_node.cached_output_high_precision) {
    node_precomputed_[node_idx].store(true, std::memory_order_release);
    return;
  }
  if (node_precomputed_[node_idx].load(std::memory_order_acquire)) {
    return;
  }

  const auto& op_opt = resolved_ops_[node_idx];
  if (!op_opt.has_value() || !std::holds_alternative<TileOpFunc>(*op_opt)) {
    throw GraphError(
        GraphErrc::NoOperation,
        "No tiled op for " + target_node.type + ":" + target_node.subtype);
  }

  Node node_for_exec;
  {
    std::lock_guard<std::mutex> lock(*output_mutexes_.at(node_idx));
    node_for_exec = target_node;
    node_for_exec.runtime_parameters = resolve_runtime_parameters(target_node);
  }
  std::vector<const NodeOutput*> inputs_ready =
      resolve_image_inputs(target_node);

  if (try_satisfy_tile_from_disk_cache(target_node, node_idx)) {
    return;
  }

  ImageBuffer* output_buffer =
      ensure_tile_output_buffer(node_idx, target_node, inputs_ready);
  if (!output_buffer) {
    return;
  }
  TiledExecutionConfig tiled_config = tiled_config_for(target_node, *op_opt);
  tiled_config.tile_size =
      task.tile_size > 0 ? task.tile_size : tiled_config.tile_size;
  tiled_config.output_roi = task.output_roi;
  tiled_config.output_size = planned_output_sizes_.at(node_idx);
  tiled_config.on_tile = nullptr;

  BenchmarkEvent current_event = start_event(target_node);
  NodeExecutor::execute_tiled_into(graph_, node_for_exec,
                                   std::get<TileOpFunc>(*op_opt), inputs_ready,
                                   *output_buffer, tiled_config);
  finalize_tiled_node_if_complete(node_idx, target_node, inputs_ready,
                                  current_event);
}

bool NodeTaskRunner::try_satisfy_tile_from_disk_cache(const Node& target_node,
                                                      int node_idx) {
  if (!allow_disk_cache()) {
    return node_precomputed_[node_idx].load(std::memory_order_acquire);
  }

  std::lock_guard<std::mutex> lock(*output_mutexes_.at(node_idx));
  if (node_precomputed_[node_idx].load(std::memory_order_acquire)) {
    return true;
  }
  if (temp_results_[node_idx].has_value()) {
    return false;
  }

  try_load_disk_cache(target_node, node_idx);
  if (temp_results_[node_idx].has_value()) {
    node_precomputed_[node_idx].store(true, std::memory_order_release);
    return true;
  }
  return false;
}

ImageBuffer* NodeTaskRunner::ensure_tile_output_buffer(
    int node_idx, const Node& target_node,
    const std::vector<const NodeOutput*>& image_inputs) {
  std::lock_guard<std::mutex> lock(*output_mutexes_.at(node_idx));
  if (node_precomputed_[node_idx].load(std::memory_order_acquire)) {
    return nullptr;
  }
  if (!temp_results_[node_idx].has_value()) {
    const PixelSize planned_size = planned_output_sizes_.at(node_idx);
    const PixelSize output_size =
        planned_size.width > 0 && planned_size.height > 0
            ? planned_size
            : PixelSize{
                  as_int_flexible(target_node.runtime_parameters, "width", 256),
                  as_int_flexible(target_node.runtime_parameters, "height",
                                  256)};
    auto [channels, dtype] = infer_tile_channels_and_type(image_inputs);
    temp_results_[node_idx] = NodeOutput{};
    temp_results_[node_idx]->image_buffer = make_aligned_cpu_image_buffer(
        output_size.width, output_size.height, channels, dtype);
  }
  return &temp_results_[node_idx]->image_buffer;
}

void NodeTaskRunner::finalize_tiled_node_if_complete(
    int node_idx, const Node& target_node,
    const std::vector<const NodeOutput*>& image_inputs,
    BenchmarkEvent& current_event) {
  const int expected_tiles = tile_task_counts_.at(node_idx);
  if (expected_tiles <= 0) {
    return;
  }
  const int previous =
      completed_tile_counts_[node_idx].fetch_add(1, std::memory_order_acq_rel);
  if (previous + 1 != expected_tiles) {
    return;
  }
  NodeOutput& output = *temp_results_[node_idx];
  const double execution_ms =
      record_computed_output(target_node, current_event);
  finalize_output_metadata(output, image_inputs, enable_timing_, execution_ms);
}

void NodeTaskRunner::try_load_disk_cache(const Node& target_node,
                                         int node_idx) {
  if (!allow_disk_cache()) {
    return;
  }
  NodeOutput from_disk;
  if (cache_.try_load_from_disk_cache_into(graph_, target_node, from_disk)) {
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

TiledExecutionConfig NodeTaskRunner::tiled_config_for(
    const Node& target_node, const OpRegistry::OpVariant& op) const {
  TiledExecutionConfig tiled_config;
  if (!std::holds_alternative<TileOpFunc>(op)) {
    return tiled_config;
  }
  if (auto metadata = OpRegistry::instance().get_metadata(
          target_node.type, target_node.subtype)) {
    tiled_config.metadata = *metadata;
    if (metadata->tile_preference == TileSizePreference::MICRO) {
      tiled_config.tile_size = 16;
    } else if (metadata->tile_preference == TileSizePreference::MACRO) {
      tiled_config.tile_size = 256;
    }
  }
  tiled_config.on_tile = [this, node_id = target_node.id](const PixelRect&) {
    task_runtime_.log_event(SchedulerTraceAction::ExecuteTile, node_id);
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
  NodeOutput result = NodeExecutor::execute(graph_, node_for_exec, *op_opt,
                                            inputs_ready, tiled_config);

  const double execution_ms =
      record_computed_output(target_node, current_event);
  finalize_output_metadata(result, inputs_ready, enable_timing_, execution_ms);
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
