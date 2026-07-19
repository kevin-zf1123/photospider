#include "compute/downsample_executor.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "core/image_buffer_processing.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Deep-copies an image buffer before downsample staging writes.
 *
 * @param source Source proxy image buffer.
 * @return Independent CPU buffer when CPU pixels are present, or a shared
 * immutable descriptor for an opaque non-CPU backend resource.
 * @throws GraphError when the selected image-processing clone implementation
 *         fails.
 * @note Empty buffers keep shape metadata but drop ownership. Opaque backend
 * descriptors are shared because the generic downsampler cannot clone or map
 * their resource; passthrough later replaces the complete proxy output.
 */
ImageBuffer clone_image_buffer(const ImageBuffer& source) {
  ImageBuffer cloned = source;
  cloned.data.reset();
  cloned.context.reset();
  if (source.width <= 0 || source.height <= 0 || source.channels <= 0 ||
      (!source.data && !source.context)) {
    return cloned;
  }
  if (source.device != Device::CPU) {
    return source;
  }
  try {
    return image_processing::clone_cpu_image_buffer(source);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Failed to clone downsample proxy buffer: " + std::string(e.what()));
  }
}

/**
 * @brief Deep-copies committed proxy node state for staged downsample writes.
 *
 * @param state Committed proxy state to copy.
 * @return Independent proxy state safe for ROI mutation before commit.
 * @throws std::bad_alloc when image or metadata copying exhausts memory.
 * @throws GraphError when image payload cloning otherwise fails.
 */
RealtimeProxyGraph::NodeState clone_proxy_state(
    const RealtimeProxyGraph::NodeState& state) {
  RealtimeProxyGraph::NodeState cloned;
  cloned.roi_hp = state.roi_hp;
  cloned.version = state.version;
  cloned.dirty_source_generation = state.dirty_source_generation;
  if (state.output) {
    NodeOutput output = *state.output;
    output.image_buffer = clone_image_buffer(state.output->image_buffer);
    cloned.output = std::move(output);
  }
  return cloned;
}

}  // namespace

/** @copydoc DownsampleExecutor::DownsampleExecutor */
DownsampleExecutor::DownsampleExecutor(GraphModel& graph,
                                       RealtimeProxyGraph& proxy_graph,
                                       GraphRuntime* runtime,
                                       GraphEventService& events)
    : graph_(graph),
      proxy_graph_(proxy_graph),
      runtime_(runtime),
      events_(events) {}  // NOLINT

/** @copydoc DownsampleExecutor::execute */
void DownsampleExecutor::execute(const std::vector<Request>& requests) {
  for (const auto& request : requests) {
    execute_one(request);
  }
}

/** @copydoc DownsampleExecutor::execute_one */
void DownsampleExecutor::execute_one(const Request& request) {
  Node* node_ptr = find_current_node(request);
  if (!node_ptr) {
    return;
  }
  Node& node = *node_ptr;
  RealtimeProxyGraph::NodeState proxy_state;
  if (const RealtimeProxyGraph::NodeState* existing =
          proxy_graph_.find_state(node.id)) {
    proxy_state = clone_proxy_state(*existing);
  }
  if (proxy_state.version > request.hp_version) {
    log_stale_generation(node.id);
    return;
  }
  const NodeOutput& hp_output = *node.cached_output_high_precision;
  const ImageBuffer& hp_buffer = hp_output.image_buffer;
  const PixelSize hp_size{std::max(hp_buffer.width, 0),
                          std::max(hp_buffer.height, 0)};
  const PixelRect roi_hp = normalize_hp_roi(request.roi_hp, hp_size);

  if (!proxy_state.output) {
    proxy_state.output = NodeOutput{};
  }
  proxy_state.output->data = hp_output.data;

  if (hp_buffer.width <= 0 || hp_buffer.height <= 0 ||
      hp_buffer.device != Device::CPU || !hp_buffer.data) {
    apply_passthrough(node, proxy_state, roi_hp, hp_size, request.hp_version);
    proxy_graph_.commit_node_state(node.id, std::move(proxy_state));
    return;
  }

  const PixelSize rt_size = scale_down_size(hp_size, kRtDownscaleFactor);
  if (rt_size.width <= 0 || rt_size.height <= 0) {
    apply_passthrough(node, proxy_state, roi_hp, hp_size, request.hp_version);
    proxy_graph_.commit_node_state(node.id, std::move(proxy_state));
    return;
  }

  ImageBuffer& rt_buffer = ensure_rt_buffer(proxy_state, hp_buffer, rt_size);
  downsample_roi(hp_buffer, rt_buffer, roi_hp, rt_size);
  commit_rt_metadata(proxy_state, roi_hp, hp_size, request.hp_version);
  proxy_graph_.commit_node_state(node.id, std::move(proxy_state));
  events_.push(node.id, node.name, "downsample", 0.0);

  if (runtime_) {
    runtime_->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE, node.id);
  }
}

/** @copydoc DownsampleExecutor::find_current_node */
Node* DownsampleExecutor::find_current_node(const Request& request) {
  Node* node_ptr = graph_.find_node_mutable(request.node_id);
  if (!node_ptr || !node_ptr->cached_output_high_precision) {
    return nullptr;
  }
  Node& node = *node_ptr;
  if (node.hp_version < request.hp_version) {
    log_stale_generation(node.id);
    return nullptr;
  }
  return node_ptr;
}

/** @copydoc DownsampleExecutor::normalize_hp_roi */
PixelRect DownsampleExecutor::normalize_hp_roi(const PixelRect& request_roi,
                                               const PixelSize& hp_size) const {
  PixelRect roi_hp = clip_rect(request_roi, hp_size);
  if (is_rect_empty(roi_hp) && hp_size.width > 0 && hp_size.height > 0) {
    roi_hp = PixelRect{0, 0, hp_size.width, hp_size.height};
  }
  return roi_hp;
}

/** @copydoc DownsampleExecutor::apply_passthrough */
void DownsampleExecutor::apply_passthrough(
    Node& node, RealtimeProxyGraph::NodeState& proxy_state,
    const PixelRect& roi_hp, const PixelSize& hp_size, int hp_version) {
  proxy_state.output = node.cached_output_high_precision;
  commit_rt_metadata(proxy_state, roi_hp, hp_size, hp_version);
  events_.push(node.id, node.name, "downsample_passthrough", 0.0);
}

/** @copydoc DownsampleExecutor::ensure_rt_buffer */
ImageBuffer& DownsampleExecutor::ensure_rt_buffer(
    RealtimeProxyGraph::NodeState& proxy_state, const ImageBuffer& hp_buffer,
    const PixelSize& rt_size) {
  if (!proxy_state.output) {
    proxy_state.output = NodeOutput{};
  }
  ImageBuffer& rt_buffer = proxy_state.output->image_buffer;
  const bool needs_alloc = (rt_buffer.width != rt_size.width) ||
                           (rt_buffer.height != rt_size.height) ||
                           (rt_buffer.channels != hp_buffer.channels) ||
                           (rt_buffer.type != hp_buffer.type) ||
                           (!rt_buffer.data);
  if (needs_alloc) {
    rt_buffer = make_aligned_cpu_image_buffer(
        rt_size.width, rt_size.height, hp_buffer.channels, hp_buffer.type);
  }
  return rt_buffer;
}

/** @copydoc DownsampleExecutor::downsample_roi */
PixelRect DownsampleExecutor::downsample_roi(const ImageBuffer& hp_buffer,
                                             ImageBuffer& rt_buffer,
                                             const PixelRect& roi_hp,
                                             const PixelSize& rt_size) const {
  PixelRect roi_rt =
      clip_rect(scale_down_rect(roi_hp, kRtDownscaleFactor), rt_size);
  if (is_rect_empty(roi_rt)) {
    roi_rt = PixelRect{0, 0, rt_size.width, rt_size.height};
  }

  image_processing::resize_cpu_image_buffer_region(hp_buffer, roi_hp, rt_buffer,
                                                   roi_rt);
  return roi_rt;
}

/** @copydoc DownsampleExecutor::commit_rt_metadata */
void DownsampleExecutor::commit_rt_metadata(
    RealtimeProxyGraph::NodeState& proxy_state, const PixelRect& roi_hp,
    const PixelSize& hp_size, int hp_version) {
  if (!is_rect_empty(roi_hp)) {
    proxy_state.roi_hp =
        proxy_state.roi_hp.has_value()
            ? clip_rect(merge_rect(*proxy_state.roi_hp, roi_hp), hp_size)
            : roi_hp;
  }
  proxy_state.version = hp_version;
}

/** @copydoc DownsampleExecutor::log_stale_generation */
void DownsampleExecutor::log_stale_generation(int node_id) const {
  if (runtime_) {
    runtime_->log_event(GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION,
                        node_id);
  }
}

}  // namespace ps::compute
