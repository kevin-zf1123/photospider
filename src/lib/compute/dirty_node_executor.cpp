#include "compute/dirty_node_executor.hpp"

#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "compute/compute_geometry.hpp"
#include "compute/dirty_execution_common.hpp"
#include "compute/dirty_update_executor.hpp"
#include "compute/domain_op_metadata.hpp"
#include "compute/node_executor.hpp"
#include "core/image_buffer_processing.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Infers staged RT output format without copying optional outputs.
 *
 * @param preferred Staged RT output already owned by the write buffer.
 * @param image_inputs Destination-indexed RT inputs, including null slots.
 * @param fallback Optional committed HP output used as a final shape hint.
 * @return Channel count and DataType for the staged RT image buffer.
 * @throws Nothing directly.
 * @note This mirrors infer_output_spec() while accepting a NodeOutput
 * reference so RT staging does not need to copy large outputs into an
 * optional. Disconnected slots are skipped only for format inference.
 */
std::pair<int, DataType> infer_staged_rt_output_spec(
    const NodeOutput& preferred,
    const std::vector<const NodeOutput*>& image_inputs,
    const std::optional<NodeOutput>& fallback) {
  const ImageBuffer& preferred_buffer = preferred.image_buffer;
  if (preferred_buffer.width > 0 && preferred_buffer.height > 0 &&
      preferred_buffer.channels > 0) {
    return {preferred_buffer.channels, preferred_buffer.type};
  }
  for (const auto* input : image_inputs) {
    if (!input) {
      continue;
    }
    const ImageBuffer& buffer = input->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  if (fallback) {
    const ImageBuffer& buffer = fallback->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  return {1, DataType::FLOAT32};
}

/**
 * @brief Checks whether one image descriptor carries a shaped payload.
 * @param buffer Image descriptor to inspect without accessing its storage.
 * @return True for positive dimensions backed by CPU data or backend context.
 * @throws Nothing.
 * @note This accepts public non-CPU context-only outputs without asking the
 * OpenCV adapter to interpret their opaque resource type.
 */
bool has_image_payload(const ImageBuffer& buffer) noexcept {
  return buffer.width > 0 && buffer.height > 0 && buffer.channels > 0 &&
         (buffer.data || buffer.context);
}

}  // namespace

HighPrecisionDirtyNodeExecutor::HighPrecisionDirtyNodeExecutor(
    DirtyNodeExecutionContext context,
    HighPrecisionDirtyWriteBuffer& hp_write_buffer)
    : graph_(context.graph),
      runtime_(context.runtime),
      events_(context.events),
      snapshot_(context.snapshot),
      dirty_generation_(context.dirty_generation),
      hp_write_buffer_(hp_write_buffer),
      node_synchronization_(context.node_synchronization) {
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

  const auto impls = OpRegistry::instance().get_implementations(
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
    NodeOutput result =
        (*hp_mono_fn)(node_for_exec, resolved_inputs.image_inputs);
    if (!has_image_payload(result.image_buffer) && result.data.empty()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Monolithic HP operator produced no output for " +
                           node_for_exec.type + ":" + node_for_exec.subtype);
    }
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    hp_write_buffer_.ensure_output(node) = std::move(result);
  } else {
    throw GraphError(GraphErrc::NoOperation,
                     "No suitable HP operator (tiled or monolithic) for " +
                         node_for_exec.type + ":" + node_for_exec.subtype);
  }

  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    commit_node(node, entry, dirty_source);
  }
}

ResolvedNodeInputs HighPrecisionDirtyNodeExecutor::resolve_inputs(
    Node& node) const {
  return NodeInputResolver::resolve(
      node,
      [&](int upstream_id) -> const NodeOutput* {
        if (const NodeOutput* staged =
                hp_write_buffer_.find_output(upstream_id)) {
          return staged;
        }
        const Node* upstream = graph_.find_node(upstream_id);
        if (!upstream) {
          return nullptr;
        }
        return ComputeCachePolicy::reusable_output(*upstream);
      },
      "HP update");
}

/**
 * @brief Executes the preferred HP implementation for one dirty node.
 *
 * @param node Node being computed.
 * @param entry HP dirty ROI and extent metadata.
 * @param image_inputs_ready Resolved HP image inputs.
 * @return Nothing.
 * @throws std::bad_alloc when staging or selected operation execution exhausts
 * memory.
 * @throws GraphError when no HP implementation exists or execution otherwise
 * fails.
 * @note Tiled HP remains preferred over monolithic HP, and output stays staged
 * until the dirty write buffer commits.
 */
void HighPrecisionDirtyNodeExecutor::execute_operation(
    Node& node, const HpPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready) const {
  const auto impls =
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
  NodeOutput& staged_output = hp_write_buffer_.ensure_output(node);
  std::optional<NodeOutput> staged_shape;
  staged_shape = staged_output;
  auto [channels, dtype] = infer_output_spec(
      staged_shape, image_inputs_ready, &node.cached_output_high_precision);
  ImageBuffer& hp_buffer = staged_output.image_buffer;
  const bool needs_alloc =
      (hp_buffer.width != entry.hp_size.width) ||
      (hp_buffer.height != entry.hp_size.height) ||
      (hp_buffer.channels != channels) || (hp_buffer.type != dtype) ||
      (hp_buffer.device != Device::CPU) || (!hp_buffer.data);
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
  NodeOutput result = mono_fn(node, image_inputs_ready);
  if (!has_image_payload(result.image_buffer) && result.data.empty()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Monolithic HP operator produced no output for " +
                         node.type + ":" + node.subtype);
  }
  hp_write_buffer_.ensure_output(node) = std::move(result);
}

void HighPrecisionDirtyNodeExecutor::commit_node(Node& node,
                                                 const HpPlanEntry& entry,
                                                 bool dirty_source) {
  hp_write_buffer_.mark_updated(node, entry.roi_hp, entry.hp_size, dirty_source,
                                dirty_generation_);
  events_.push(node.id, node.name, "hp_update", 0.0);
}

bool HighPrecisionDirtyNodeExecutor::should_skip_node(const Node& node,
                                                      bool dirty_source) const {
  uint64_t committed_generation = 0;
  auto generation_it = graph_.dirty_source_hp_commit_generation.find(node.id);
  if (generation_it != graph_.dirty_source_hp_commit_generation.end()) {
    committed_generation = generation_it->second;
  }
  return dirty_source &&
         should_skip_stale_dirty_source(runtime_, node.id, committed_generation,
                                        dirty_generation_);
}

std::mutex& HighPrecisionDirtyNodeExecutor::node_mutex(int node_id) const {
  return node_synchronization_.mutex_for(node_id);
}

RealTimeDirtyNodeExecutor::RealTimeDirtyNodeExecutor(
    DirtyNodeExecutionContext context, RealtimeProxyGraph& proxy_graph,
    RealtimeProxyWriteBuffer& rt_write_buffer)
    : graph_(context.graph),
      runtime_(context.runtime),
      events_(context.events),
      snapshot_(context.snapshot),
      dirty_generation_(context.dirty_generation),
      stabilized_parameters_(context.stabilized_parameters),
      proxy_graph_(proxy_graph),
      rt_write_buffer_(rt_write_buffer),
      node_synchronization_(context.node_synchronization) {
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
    throw GraphError(GraphErrc::NoOperation,
                     "No operator registered for node " + node_for_exec.type +
                         ":" + node_for_exec.subtype);
  }
  if (std::holds_alternative<MonolithicOpFunc>(*op_variant)) {
    NodeOutput result = std::get<MonolithicOpFunc>(*op_variant)(
        node_for_exec, resolved_inputs.image_inputs);
    if (!has_image_payload(result.image_buffer) && result.data.empty()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Monolithic RT operator produced no output for " +
                           node_for_exec.type + ":" + node_for_exec.subtype);
    }
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    if (result.image_buffer.device != Device::CPU &&
        has_image_payload(result.image_buffer)) {
      rt_write_buffer_.ensure_output(node.id) = std::move(result);
    } else {
      ImageBuffer& rt_buffer =
          ensure_rt_buffer(node, entry, resolved_inputs.image_inputs);
      copy_monolithic_image_roi(result, entry, rt_buffer);
      rt_write_buffer_.ensure_output(node.id).data = std::move(result.data);
    }
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
  const NodeInputResolver::OutputLookup image_lookup =
      [&](int upstream_id) -> const NodeOutput* {
    if (const NodeOutput* staged = rt_write_buffer_.find_output(upstream_id)) {
      return staged;
    }
    if (const NodeOutput* proxy_output =
            proxy_graph_.find_output(upstream_id)) {
      return proxy_output;
    }
    const Node* upstream = graph_.find_node(upstream_id);
    if (!upstream) {
      return nullptr;
    }
    return ComputeCachePolicy::reusable_output(*upstream);
  };
  const NodeInputResolver::OutputLookup parameter_lookup =
      [&](int upstream_id) -> const NodeOutput* {
    if (stabilized_parameters_) {
      if (const NodeOutput* stabilized =
              stabilized_parameters_->find_parameter_output(upstream_id)) {
        return stabilized;
      }
    }
    return image_lookup(upstream_id);
  };
  return NodeInputResolver::resolve(node, image_lookup, parameter_lookup,
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
    const Node& node, const RtPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready) const {
  NodeOutput& staged_output = rt_write_buffer_.ensure_output(node.id);
  auto [channels, dtype] = infer_staged_rt_output_spec(
      staged_output, image_inputs_ready, node.cached_output_high_precision);
  ImageBuffer& rt_buffer = staged_output.image_buffer;
  const bool needs_alloc =
      (rt_buffer.width != entry.rt_size.width) ||
      (rt_buffer.height != entry.rt_size.height) ||
      (rt_buffer.channels != channels) || (rt_buffer.type != dtype) ||
      (rt_buffer.device != Device::CPU) || (!rt_buffer.data);
  if (needs_alloc) {
    rt_buffer = make_aligned_cpu_image_buffer(
        entry.rt_size.width, entry.rt_size.height, channels, dtype);
  }
  return rt_buffer;
}

/**
 * @brief Executes the selected RT or HP-fallback implementation.
 *
 * @param node Node being computed.
 * @param entry RT dirty ROI and extent metadata.
 * @param image_inputs_ready Resolved RT image inputs.
 * @param rt_buffer Staged destination proxy buffer.
 * @param op_variant Selected monolithic or tiled operation.
 * @return Nothing.
 * @throws std::bad_alloc when operation execution or staging exhausts memory.
 * @throws GraphError preserving operation errors and wrapping other standard
 * or selected image-processing failures with node context.
 * @note Resource exhaustion retains its type through dirty execution and the
 * public Host boundary; ordinary failures retain RT diagnostic wrapping.
 */
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
  } catch (const std::bad_alloc&) {
    throw;
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
  if (!has_image_payload(result.image_buffer) && result.data.empty()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Monolithic RT operator produced no output for " +
                         node.type + ":" + node.subtype);
  }
  if (result.image_buffer.device != Device::CPU &&
      has_image_payload(result.image_buffer)) {
    rt_write_buffer_.ensure_output(node.id) = std::move(result);
    return;
  }
  copy_monolithic_image_roi(result, entry, rt_buffer);
  rt_write_buffer_.ensure_output(node.id).data = std::move(result.data);
}

/** @copydoc RealTimeDirtyNodeExecutor::copy_monolithic_image_roi */
void RealTimeDirtyNodeExecutor::copy_monolithic_image_roi(
    const NodeOutput& result, const RtPlanEntry& entry,
    ImageBuffer& rt_buffer) const {
  if (result.image_buffer.width <= 0 || result.image_buffer.height <= 0) {
    return;
  }
  if (result.image_buffer.device != Device::CPU) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Opaque backend monolithic output must replace the full RT result");
  }

  if (rt_buffer.width != entry.rt_size.width ||
      rt_buffer.height != entry.rt_size.height ||
      rt_buffer.channels != result.image_buffer.channels ||
      rt_buffer.type != result.image_buffer.type || !rt_buffer.data) {
    rt_buffer = make_aligned_cpu_image_buffer(
        entry.rt_size.width, entry.rt_size.height, result.image_buffer.channels,
        result.image_buffer.type);
  }

  const ImageBuffer* normalized_result = &result.image_buffer;
  std::optional<ImageBuffer> resized_result;
  const bool needs_resize = result.image_buffer.width != entry.rt_size.width ||
                            result.image_buffer.height != entry.rt_size.height;
  if (needs_resize) {
    resized_result = image_processing::resize_cpu_image_buffer(
        result.image_buffer, entry.rt_size);
    normalized_result = &*resized_result;
  }
  copy_image_buffer_region(InputTileView{normalized_result, entry.roi_rt},
                           OutputTileView{&rt_buffer, entry.roi_rt});
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
          metadata_for_domain(node.type, node.subtype, DirtyDomain::RealTime)) {
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
  rt_write_buffer_.mark_updated(node.id, entry.roi_hp, entry.hp_size,
                                dirty_source, dirty_generation_);
  events_.push(node.id, node.name, "rt_update", 0.0);
}

bool RealTimeDirtyNodeExecutor::should_skip_node(const Node& node,
                                                 bool dirty_source) const {
  const uint64_t committed_generation =
      proxy_graph_.dirty_source_generation(node.id);
  return dirty_source &&
         should_skip_stale_dirty_source(runtime_, node.id, committed_generation,
                                        dirty_generation_);
}

std::mutex& RealTimeDirtyNodeExecutor::node_mutex(int node_id) const {
  return node_synchronization_.mutex_for(node_id);
}

}  // namespace ps::compute
