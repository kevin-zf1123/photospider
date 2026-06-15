#include "kernel/services/compute-service/compute_node_task_runner.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kernel/services/compute-service/compute_metrics_recorder.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"

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

}  // namespace

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
      force_recache_(context.force_recache),
      enable_timing_(context.enable_timing),
      disable_disk_cache_(context.disable_disk_cache),
      benchmark_events_(context.benchmark_events) {
}  // NOLINT(whitespace/indent_namespace)

void NodeTaskRunner::run_node(int node_idx) {
  const int node_id = execution_order_.at(node_idx);
  try {
    compute_node(node_idx, node_id);
  } catch (const cv::Exception& e) {
    std::rethrow_exception(compute_failure(graph_, node_id, e.what()));
  } catch (const std::exception& e) {
    std::rethrow_exception(compute_failure(graph_, node_id, e.what()));
  } catch (...) {
    std::rethrow_exception(
        compute_failure(graph_, node_id, "unknown exception"));
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

YAML::Node NodeTaskRunner::resolve_runtime_parameters(
    const Node& target_node) const {
  YAML::Node runtime_params = target_node.parameters
                                  ? YAML::Clone(target_node.parameters)
                                  : YAML::Node(YAML::NodeType::Map);
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
    runtime_params[p_input.to_parameter_name] = it->second;
  }
  return runtime_params;
}

std::vector<const NodeOutput*> NodeTaskRunner::resolve_image_inputs(
    const Node& target_node) const {
  std::vector<const NodeOutput*> inputs_ready;
  inputs_ready.reserve(target_node.image_inputs.size());
  for (const auto& i_input : target_node.image_inputs) {
    if (i_input.from_node_id < 0) {
      continue;
    }
    auto const* up_out = upstream_output(i_input.from_node_id);
    if (!up_out) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "Image input not ready for node " + std::to_string(target_node.id));
    }
    inputs_ready.push_back(up_out);
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
  tiled_config.on_tile = [this, node_id = target_node.id](const cv::Rect&) {
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

  YAML::Node runtime_params = resolve_runtime_parameters(target_node);
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
