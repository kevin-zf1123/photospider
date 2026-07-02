#include "kernel/services/compute-service/realtime_dirty_write_buffer.hpp"

#include <string>
#include <utility>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Deep-copies an image buffer so staged writes cannot alias graph state.
 *
 * @param source Source image buffer from a graph-owned NodeOutput.
 * @return Independent ImageBuffer with copied pixels when source has payload,
 * otherwise metadata with no owned data or API context.
 * @throws GraphError when adapter conversion fails; may throw std::bad_alloc
 * while allocating the cloned OpenCV matrix.
 * @note The clone goes through the OpenCV adapter so CPU and UMat-backed
 * buffers are both copied into independent CPU-backed storage.
 */
ImageBuffer clone_image_buffer(const ImageBuffer& source) {
  ImageBuffer cloned = source;
  cloned.data.reset();
  cloned.context.reset();
  if (source.width <= 0 || source.height <= 0 || source.channels <= 0 ||
      (!source.data && !source.context)) {
    return cloned;
  }

  try {
    cloned = fromCvMat(toCvMat(source).clone());
  } catch (const std::exception& e) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Failed to clone RT staged image buffer: " + std::string(e.what()));
  }
  return cloned;
}

/**
 * @brief Deep-copies a node output for RT staging.
 *
 * @param source Graph-owned output used as the initial staged state.
 * @return Independent output with cloned image payload and copied metadata.
 * @throws GraphError when image payload cloning fails.
 * @note YAML data, spatial context, and debug metadata are value-copied.
 */
NodeOutput clone_node_output(const NodeOutput& source) {
  NodeOutput cloned;
  cloned.image_buffer = clone_image_buffer(source.image_buffer);
  cloned.data = source.data;
  cloned.space = source.space;
  cloned.debug = source.debug;
  return cloned;
}

}  // namespace

const NodeOutput* RealtimeDirtyWriteBuffer::find_output(int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(node_id);
  if (it == entries_.end() || !it->second.has_output) {
    return nullptr;
  }
  return &it->second.output;
}

bool RealtimeDirtyWriteBuffer::has_output(int node_id) const {
  return find_output(node_id) != nullptr;
}

NodeOutput& RealtimeDirtyWriteBuffer::ensure_output(const Node& node) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = entries_[node.id];
  if (!entry.initialized) {
    entry.initialized = true;
    entry.rt_roi = node.rt_roi;
    entry.rt_version = node.rt_version;
    if (node.cached_output_real_time) {
      entry.output = clone_node_output(*node.cached_output_real_time);
      entry.has_output = true;
    }
  }
  if (!entry.has_output) {
    entry.output = NodeOutput{};
    entry.has_output = true;
  }
  return entry.output;
}

void RealtimeDirtyWriteBuffer::mark_updated(int node_id, const cv::Rect& roi_hp,
                                            const cv::Size& hp_size,
                                            bool dirty_source,
                                            uint64_t dirty_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = entries_[node_id];
  if (!entry.initialized) {
    entry.initialized = true;
  }
  if (!is_rect_empty(roi_hp)) {
    entry.rt_roi = entry.rt_roi.has_value()
                       ? clip_rect(merge_rect(*entry.rt_roi, roi_hp), hp_size)
                       : roi_hp;
  }
  entry.rt_version++;
  if (dirty_source) {
    entry.dirty_source_generation = dirty_generation;
  }
}

void RealtimeDirtyWriteBuffer::commit_to_graph(GraphModel& graph) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : entries_) {
    const int node_id = item.first;
    Entry& entry = item.second;
    if (!entry.has_output) {
      continue;
    }
    graph.mutate_node_runtime_state(
        node_id, [&](GraphModel::NodeRuntimeState& state) {
          state.cached_output_real_time = std::move(entry.output);
          state.rt_roi = entry.rt_roi;
          state.rt_version = entry.rt_version;
        });
    if (entry.dirty_source_generation) {
      graph.dirty_source_rt_commit_generation[node_id] =
          *entry.dirty_source_generation;
    }
  }
}

}  // namespace ps::compute
