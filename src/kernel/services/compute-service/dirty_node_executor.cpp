#include "kernel/services/compute-service/dirty_node_executor.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/dirty_execution_common.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/graph_event_service.hpp"

namespace ps::compute {

HighPrecisionDirtyNodeExecutor::HighPrecisionDirtyNodeExecutor(
    DirtyNodeExecutionContext context, DownsampleRequestSink downsample_sink)
    : graph_(context.graph),
      runtime_(context.runtime),
      events_(context.events),
      snapshot_(context.snapshot),
      dirty_generation_(context.dirty_generation),
      downsample_requests_(downsample_sink.requests),
      downsample_requests_mutex_(downsample_sink.mutex),
      node_mutexes_(context.node_mutexes) {
}  // NOLINT(whitespace/indent_namespace)

void HighPrecisionDirtyNodeExecutor::execute(Node& node,
                                             const HpPlanEntry& entry) {
  if (is_rect_empty(entry.roi_hp)) {
    return;
  }
  const bool dirty_source = is_dirty_source_node(snapshot_, node.id);
  if (should_skip_node(node, dirty_source)) {
    return;
  }

  log_dirty_node_execution(runtime_, node.id, dirty_source);
  Node node_for_exec;
  ResolvedNodeInputs resolved_inputs;
  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    node_for_exec = node;
    resolved_inputs = resolve_inputs(node_for_exec);
  }

  const auto* impls = OpRegistry::instance().get_implementations(
      node_for_exec.type, node_for_exec.subtype);
  const TileOpFunc* hp_tile_fn =
      (impls && impls->tiled_hp) ? &*impls->tiled_hp : nullptr;
  const MonolithicOpFunc* hp_mono_fn =
      (impls && impls->monolithic_hp) ? &*impls->monolithic_hp : nullptr;

  if (hp_tile_fn) {
    ImageBuffer* hp_buffer = nullptr;
    {
      std::lock_guard<std::mutex> lock(node_mutex(node.id));
      hp_buffer = &ensure_hp_buffer(node, entry, resolved_inputs.image_inputs);
    }
    execute_tiled(node_for_exec, *hp_tile_fn, entry,
                  resolved_inputs.image_inputs, *hp_buffer);
  } else if (hp_mono_fn) {
    NodeOutput result = (*hp_mono_fn)(node_for_exec,
                                      resolved_inputs.image_inputs);
    if (!result.image_buffer.data && result.data.empty()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Monolithic HP operator produced no output for " +
                           node_for_exec.type + ":" + node_for_exec.subtype);
    }
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    node.cached_output_high_precision = std::move(result);
  } else {
    throw GraphError(GraphErrc::NoOperation,
                     "No suitable HP operator (tiled or monolithic) for " +
                         node_for_exec.type + ":" + node_for_exec.subtype);
  }

  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    commit_node(node, entry, dirty_source);
    queue_downsample_request(node, entry);
  }
}

ResolvedNodeInputs HighPrecisionDirtyNodeExecutor::resolve_inputs(
    Node& node) const {
  return NodeInputResolver::resolve(
      node,
      [&](int upstream_id) -> const NodeOutput* {
        const Node* upstream = graph_.find_node(upstream_id);
        if (!upstream) {
          return nullptr;
        }
        return ComputeCachePolicy::reusable_output(*upstream);
      },
      "HP update");
}

void HighPrecisionDirtyNodeExecutor::execute_operation(
    Node& node, const HpPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready) const {
  const auto* impls =
      OpRegistry::instance().get_implementations(node.type, node.subtype);
  const TileOpFunc* hp_tile_fn =
      (impls && impls->tiled_hp) ? &*impls->tiled_hp : nullptr;
  const MonolithicOpFunc* hp_mono_fn =
      (impls && impls->monolithic_hp) ? &*impls->monolithic_hp : nullptr;

  if (hp_tile_fn) {
    ImageBuffer& hp_buffer = ensure_hp_buffer(node, entry, image_inputs_ready);
    execute_tiled(node, *hp_tile_fn, entry, image_inputs_ready, hp_buffer);
    return;
  }
  if (hp_mono_fn) {
    execute_monolithic(node, *hp_mono_fn, image_inputs_ready);
    return;
  }
  throw GraphError(GraphErrc::NoOperation,
                   "No suitable HP operator (tiled or monolithic) for " +
                       node.type + ":" + node.subtype);
}

ImageBuffer& HighPrecisionDirtyNodeExecutor::ensure_hp_buffer(
    Node& node, const HpPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready) const {
  auto [channels, dtype] =
      infer_output_spec(node.cached_output_high_precision, image_inputs_ready);
  if (!node.cached_output_high_precision) {
    node.cached_output_high_precision = NodeOutput{};
  }
  ImageBuffer& hp_buffer = node.cached_output_high_precision->image_buffer;
  const bool needs_alloc = (hp_buffer.width != entry.hp_size.width) ||
                           (hp_buffer.height != entry.hp_size.height) ||
                           (hp_buffer.channels != channels) ||
                           (hp_buffer.type != dtype) || (!hp_buffer.data);
  if (needs_alloc) {
    hp_buffer = make_aligned_cpu_image_buffer(
        entry.hp_size.width, entry.hp_size.height, channels, dtype);
  }
  return hp_buffer;
}

void HighPrecisionDirtyNodeExecutor::execute_tiled(
    Node& node, const TileOpFunc& tile_fn, const HpPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready,
    ImageBuffer& hp_buffer) const {
  TiledExecutionConfig config;
  config.tile_size = kHpMicroTileSize;
  config.output_roi = entry.roi_hp;
  config.output_size = entry.hp_size;
  config.forced_halo = entry.halo_hp;
  if (auto metadata =
          OpRegistry::instance().get_metadata(node.type, node.subtype)) {
    config.metadata = *metadata;
  }
  if (runtime_) {
    runtime_->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE, node.id);
    runtime_->log_event(
        GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE, node.id);
  }
  NodeExecutor::execute_tiled_into(graph_, node, tile_fn, image_inputs_ready,
                                   hp_buffer, config);
}

void HighPrecisionDirtyNodeExecutor::execute_monolithic(
    Node& node, const MonolithicOpFunc& mono_fn,
    const std::vector<const NodeOutput*>& image_inputs_ready) const {
  node.cached_output_high_precision = mono_fn(node, image_inputs_ready);
  if (!node.cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "Monolithic HP operator produced no output for " +
                         node.type + ":" + node.subtype);
  }
}

void HighPrecisionDirtyNodeExecutor::commit_node(Node& node,
                                                 const HpPlanEntry& entry,
                                                 bool dirty_source) {
  node.hp_roi =
      node.hp_roi.has_value()
          ? clip_rect(merge_rect(*node.hp_roi, entry.roi_hp), entry.hp_size)
          : entry.roi_hp;
  node.hp_version++;
  if (dirty_source) {
    graph_.dirty_source_hp_commit_generation[node.id] = dirty_generation_;
  }
  events_.push(node.id, node.name, "hp_update", 0.0);
}

void HighPrecisionDirtyNodeExecutor::queue_downsample_request(
    const Node& node, const HpPlanEntry& entry) {
  if (!runtime_) {
    return;
  }
  std::lock_guard<std::mutex> lock(downsample_requests_mutex_);
  downsample_requests_.push_back({node.id, entry.roi_hp, node.hp_version});
}

bool HighPrecisionDirtyNodeExecutor::should_skip_node(const Node& node,
                                                      bool dirty_source) const {
  return dirty_source && should_skip_stale_dirty_source(
                             runtime_, node.id,
                             graph_.dirty_source_hp_commit_generation[node.id],
                             dirty_generation_);
}

std::mutex& HighPrecisionDirtyNodeExecutor::node_mutex(int node_id) const {
  return *node_mutexes_.at(node_id);
}

RealTimeDirtyNodeExecutor::RealTimeDirtyNodeExecutor(
    DirtyNodeExecutionContext context)
    : graph_(context.graph),
      runtime_(context.runtime),
      events_(context.events),
      snapshot_(context.snapshot),
      dirty_generation_(context.dirty_generation),
      node_mutexes_(context.node_mutexes) {
}  // NOLINT(whitespace/indent_namespace)

void RealTimeDirtyNodeExecutor::execute(Node& node, const RtPlanEntry& entry) {
  if (is_rect_empty(entry.roi_rt)) {
    return;
  }
  const bool dirty_source = is_dirty_source_node(snapshot_, node.id);
  if (should_skip_node(node, dirty_source)) {
    return;
  }

  log_dirty_node_execution(runtime_, node.id, dirty_source);
  Node node_for_exec;
  ResolvedNodeInputs resolved_inputs;
  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    node_for_exec = node;
    resolved_inputs = resolve_inputs(node_for_exec);
  }
  std::optional<OpRegistry::OpVariant> op_variant =
      resolve_operation(node_for_exec);
  if (!op_variant) {
    throw GraphError(
        GraphErrc::NoOperation,
        "No operator registered for node " + node_for_exec.type + ":" +
            node_for_exec.subtype);
  }
  if (std::holds_alternative<MonolithicOpFunc>(*op_variant)) {
    NodeOutput result =
        std::get<MonolithicOpFunc>(*op_variant)(node_for_exec,
                                                resolved_inputs.image_inputs);
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    ImageBuffer& rt_buffer =
        ensure_rt_buffer(node, entry, resolved_inputs.image_inputs);
    copy_monolithic_image_roi(result, entry, rt_buffer);
    node.cached_output_real_time->data = result.data;
  } else {
    ImageBuffer* rt_buffer = nullptr;
    {
      std::lock_guard<std::mutex> lock(node_mutex(node.id));
      rt_buffer = &ensure_rt_buffer(node, entry, resolved_inputs.image_inputs);
    }
    execute_tiled(node_for_exec, std::get<TileOpFunc>(*op_variant), entry,
                  resolved_inputs.image_inputs, *rt_buffer);
  }

  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    commit_node(node, entry, dirty_source);
  }
}

ResolvedNodeInputs RealTimeDirtyNodeExecutor::resolve_inputs(Node& node) const {
  return NodeInputResolver::resolve(
      node,
      [&](int upstream_id) -> const NodeOutput* {
        const Node* upstream = graph_.find_node(upstream_id);
        if (!upstream) {
          return nullptr;
        }
        return ComputeCachePolicy::interactive_output(*upstream);
      },
      "RT update");
}

std::optional<OpRegistry::OpVariant>
RealTimeDirtyNodeExecutor::resolve_operation(const Node& node) const {
  auto op_variant = OpRegistry::instance().resolve_for_intent(
      node.type, node.subtype, ComputeIntent::RealTimeUpdate);
  if (op_variant) {
    return op_variant;
  }
  return OpRegistry::instance().resolve_for_intent(
      node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
}

ImageBuffer& RealTimeDirtyNodeExecutor::ensure_rt_buffer(
    Node& node, const RtPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready) const {
  auto [channels, dtype] =
      infer_output_spec(node.cached_output_real_time, image_inputs_ready,
                        &node.cached_output_high_precision);
  if (!node.cached_output_real_time) {
    node.cached_output_real_time = NodeOutput{};
  }
  ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
  const bool needs_alloc = (rt_buffer.width != entry.rt_size.width) ||
                           (rt_buffer.height != entry.rt_size.height) ||
                           (rt_buffer.channels != channels) ||
                           (rt_buffer.type != dtype) || (!rt_buffer.data);
  if (needs_alloc) {
    rt_buffer = make_aligned_cpu_image_buffer(
        entry.rt_size.width, entry.rt_size.height, channels, dtype);
  }
  return rt_buffer;
}

void RealTimeDirtyNodeExecutor::execute_operation(
    Node& node, const RtPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready,
    ImageBuffer& rt_buffer, const OpRegistry::OpVariant& op_variant) const {
  try {
    if (std::holds_alternative<MonolithicOpFunc>(op_variant)) {
      execute_monolithic(node, entry, image_inputs_ready, rt_buffer,
                         std::get<MonolithicOpFunc>(op_variant));
      return;
    }
    execute_tiled(node, std::get<TileOpFunc>(op_variant), entry,
                  image_inputs_ready, rt_buffer);
  } catch (const cv::Exception& e) {
    throw GraphError(GraphErrc::ComputeError, "RT compute failed at node " +
                                                  std::to_string(node.id) +
                                                  ": " + std::string(e.what()));
  } catch (const GraphError&) {
    throw;
  } catch (const std::exception& e) {
    throw GraphError(GraphErrc::ComputeError, "RT compute failed at node " +
                                                  std::to_string(node.id) +
                                                  ": " + std::string(e.what()));
  }
}

void RealTimeDirtyNodeExecutor::execute_monolithic(
    Node& node, const RtPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready,
    ImageBuffer& rt_buffer, const MonolithicOpFunc& mono_fn) const {
  NodeOutput result = mono_fn(node, image_inputs_ready);
  copy_monolithic_image_roi(result, entry, rt_buffer);
  node.cached_output_real_time->data = result.data;
}

void RealTimeDirtyNodeExecutor::copy_monolithic_image_roi(
    const NodeOutput& result, const RtPlanEntry& entry,
    ImageBuffer& rt_buffer) const {
  if (result.image_buffer.width <= 0 || result.image_buffer.height <= 0) {
    return;
  }

  if (rt_buffer.width != entry.rt_size.width ||
      rt_buffer.height != entry.rt_size.height ||
      rt_buffer.channels != result.image_buffer.channels ||
      rt_buffer.type != result.image_buffer.type || !rt_buffer.data) {
    rt_buffer = make_aligned_cpu_image_buffer(
        entry.rt_size.width, entry.rt_size.height, result.image_buffer.channels,
        result.image_buffer.type);
  }

  cv::Mat result_mat = toCvMat(result.image_buffer);
  cv::Mat resized_result;
  const bool needs_resize = result_mat.cols != entry.rt_size.width ||
                            result_mat.rows != entry.rt_size.height;
  if (needs_resize) {
    cv::resize(result_mat, resized_result,
               cv::Size(entry.rt_size.width, entry.rt_size.height), 0, 0,
               cv::INTER_LINEAR);
    result_mat = resized_result;
  }
  cv::Mat dest = toCvMat(rt_buffer);
  result_mat(entry.roi_rt).copyTo(dest(entry.roi_rt));
}

void RealTimeDirtyNodeExecutor::execute_tiled(
    Node& node, const TileOpFunc& tile_fn, const RtPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready,
    ImageBuffer& rt_buffer) const {
  TiledExecutionConfig config;
  config.tile_size = kRtTileSize;
  config.output_roi = entry.roi_rt;
  config.output_size = entry.rt_size;
  config.forced_halo = entry.halo_rt;
  if (auto metadata =
          OpRegistry::instance().get_metadata(node.type, node.subtype)) {
    config.metadata = *metadata;
  }
  NodeExecutor::execute_tiled_into(graph_, node, tile_fn, image_inputs_ready,
                                   rt_buffer, config);
  if (runtime_) {
    runtime_->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE, node.id);
    runtime_->log_event(
        GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE, node.id);
  }
}

void RealTimeDirtyNodeExecutor::commit_node(Node& node,
                                            const RtPlanEntry& entry,
                                            bool dirty_source) {
  if (node.rt_roi.has_value()) {
    node.rt_roi =
        clip_rect(merge_rect(*node.rt_roi, entry.roi_hp), entry.hp_size);
  } else {
    node.rt_roi = entry.roi_hp;
  }
  node.rt_version++;
  if (dirty_source) {
    graph_.dirty_source_rt_commit_generation[node.id] = dirty_generation_;
  }
  events_.push(node.id, node.name, "rt_update", 0.0);
}

bool RealTimeDirtyNodeExecutor::should_skip_node(const Node& node,
                                                 bool dirty_source) const {
  return dirty_source && should_skip_stale_dirty_source(
                             runtime_, node.id,
                             graph_.dirty_source_rt_commit_generation[node.id],
                             dirty_generation_);
}

std::mutex& RealTimeDirtyNodeExecutor::node_mutex(int node_id) const {
  return *node_mutexes_.at(node_id);
}

}  // namespace ps::compute
