#include "kernel/services/compute-service/downsample_executor.hpp"

#include <algorithm>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/graph_event_service.hpp"

namespace ps::compute {

DownsampleExecutor::DownsampleExecutor(GraphModel& graph, GraphRuntime* runtime,
                                       GraphEventService& events)
    : graph_(graph), runtime_(runtime), events_(events) {}

void DownsampleExecutor::execute(const std::vector<Request>& requests) {
  for (const auto& request : requests) {
    execute_one(request);
  }
}

void DownsampleExecutor::execute_one(const Request& request) {
  Node* node_ptr = find_current_node(request);
  if (!node_ptr) {
    return;
  }
  Node& node = *node_ptr;
  const NodeOutput& hp_output = *node.cached_output_high_precision;
  const ImageBuffer& hp_buffer = hp_output.image_buffer;
  cv::Size hp_size(std::max(hp_buffer.width, 0), std::max(hp_buffer.height, 0));
  cv::Rect roi_hp = normalize_hp_roi(request.roi_hp, hp_size);

  if (!node.cached_output_real_time) {
    node.cached_output_real_time = NodeOutput{};
  }
  node.cached_output_real_time->data = hp_output.data;

  if (hp_buffer.width <= 0 || hp_buffer.height <= 0 || !hp_buffer.data) {
    apply_passthrough(node, roi_hp, hp_size, request.hp_version);
    return;
  }

  cv::Size rt_size = scale_down_size(hp_size, kRtDownscaleFactor);
  if (rt_size.width <= 0 || rt_size.height <= 0) {
    apply_passthrough(node, roi_hp, hp_size, request.hp_version);
    return;
  }

  ImageBuffer& rt_buffer = ensure_rt_buffer(node, hp_buffer, rt_size);
  downsample_roi(hp_buffer, rt_buffer, roi_hp, rt_size);
  commit_rt_metadata(node, roi_hp, hp_size, request.hp_version);
  events_.push(node.id, node.name, "downsample", 0.0);

  if (runtime_) {
    runtime_->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE, node.id);
  }
}

Node* DownsampleExecutor::find_current_node(const Request& request) {
  Node* node_ptr = graph_.find_node_mutable(request.node_id);
  if (!node_ptr || !node_ptr->cached_output_high_precision) {
    return nullptr;
  }
  Node& node = *node_ptr;
  if (node.hp_version < request.hp_version ||
      node.rt_version > request.hp_version) {
    log_stale_generation(node.id);
    return nullptr;
  }
  return node_ptr;
}

cv::Rect DownsampleExecutor::normalize_hp_roi(const cv::Rect& request_roi,
                                              const cv::Size& hp_size) const {
  cv::Rect roi_hp = clip_rect(request_roi, hp_size);
  if (is_rect_empty(roi_hp) && hp_size.width > 0 && hp_size.height > 0) {
    roi_hp = cv::Rect(0, 0, hp_size.width, hp_size.height);
  }
  return roi_hp;
}

void DownsampleExecutor::apply_passthrough(Node& node, const cv::Rect& roi_hp,
                                           const cv::Size& hp_size,
                                           int hp_version) {
  node.cached_output_real_time = node.cached_output_high_precision;
  commit_rt_metadata(node, roi_hp, hp_size, hp_version);
  events_.push(node.id, node.name, "downsample_passthrough", 0.0);
}

ImageBuffer& DownsampleExecutor::ensure_rt_buffer(Node& node,
                                                  const ImageBuffer& hp_buffer,
                                                  const cv::Size& rt_size) {
  ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
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

cv::Rect DownsampleExecutor::downsample_roi(const ImageBuffer& hp_buffer,
                                            ImageBuffer& rt_buffer,
                                            const cv::Rect& roi_hp,
                                            const cv::Size& rt_size) const {
  cv::Rect roi_rt =
      clip_rect(scale_down_rect(roi_hp, kRtDownscaleFactor), rt_size);
  if (is_rect_empty(roi_rt)) {
    roi_rt = cv::Rect(0, 0, rt_size.width, rt_size.height);
  }

  cv::Mat hp_mat = toCvMat(hp_buffer);
  cv::Mat rt_mat = toCvMat(rt_buffer);
  cv::Mat hp_patch = hp_mat(roi_hp);
  cv::Mat downsampled;
  cv::resize(hp_patch, downsampled, cv::Size(roi_rt.width, roi_rt.height), 0, 0,
             cv::INTER_LINEAR);
  downsampled.copyTo(rt_mat(roi_rt));
  return roi_rt;
}

void DownsampleExecutor::commit_rt_metadata(Node& node, const cv::Rect& roi_hp,
                                            const cv::Size& hp_size,
                                            int hp_version) {
  if (!is_rect_empty(roi_hp)) {
    node.rt_roi = node.rt_roi.has_value()
                      ? clip_rect(merge_rect(*node.rt_roi, roi_hp), hp_size)
                      : roi_hp;
  }
  node.rt_version = hp_version;
}

void DownsampleExecutor::log_stale_generation(int node_id) const {
  if (runtime_) {
    runtime_->log_event(GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION,
                        node_id);
  }
}

}  // namespace ps::compute
