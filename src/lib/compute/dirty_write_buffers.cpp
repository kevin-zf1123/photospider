#include "kernel/services/compute-service/dirty_write_buffers.hpp"

#include <new>
#include <string>
#include <utility>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Deep-copies an image buffer for staged dirty writes.
 *
 * @param source Source image buffer owned by graph or proxy state.
 * @param label Human-readable buffer domain used in error messages.
 * @return Independent ImageBuffer with cloned image payload when present.
 * @throws GraphError when adapter conversion fails; may throw std::bad_alloc
 * while allocating cloned OpenCV storage.
 * @note Empty buffers keep shape metadata but drop data/context ownership.
 */
ImageBuffer clone_image_buffer(const ImageBuffer& source,
                               const std::string& label) {
  ImageBuffer cloned = source;
  cloned.data.reset();
  cloned.context.reset();
  if (source.width <= 0 || source.height <= 0 || source.channels <= 0 ||
      (!source.data && !source.context)) {
    return cloned;
  }

  try {
    cloned = fromCvMat(toCvMat(source).clone());
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    throw GraphError(GraphErrc::ComputeError,
                     "Failed to clone " + label +
                         " staged image buffer: " + std::string(e.what()));
  }
  return cloned;
}

/**
 * @brief Deep-copies a NodeOutput for staged dirty writes.
 *
 * @param source Source output owned by graph or proxy state.
 * @param label Human-readable buffer domain used in error messages.
 * @return Independent output with cloned image payload and copied metadata.
 * @throws std::bad_alloc when output or metadata copying exhausts memory.
 * @throws GraphError when image payload cloning otherwise fails.
 * @note YAML payload, spatial context, and debug metadata are value-copied.
 */
NodeOutput clone_node_output(const NodeOutput& source,
                             const std::string& label) {
  NodeOutput cloned;
  cloned.image_buffer = clone_image_buffer(source.image_buffer, label);
  cloned.data = source.data;
  cloned.space = source.space;
  cloned.debug = source.debug;
  return cloned;
}

}  // namespace

HighPrecisionDirtyWriteBuffer::HighPrecisionDirtyWriteBuffer(
    bool seed_existing_outputs)
    : seed_existing_outputs_(seed_existing_outputs) {}

const NodeOutput* HighPrecisionDirtyWriteBuffer::find_output(
    int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(node_id);
  if (it == entries_.end() || !it->second.has_output) {
    return nullptr;
  }
  return &it->second.output;
}

bool HighPrecisionDirtyWriteBuffer::has_output(int node_id) const {
  return find_output(node_id) != nullptr;
}

NodeOutput& HighPrecisionDirtyWriteBuffer::ensure_output(const Node& node) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node);
  if (!entry.has_output) {
    entry.output = NodeOutput{};
    entry.has_output = true;
  }
  return entry.output;
}

int HighPrecisionDirtyWriteBuffer::mark_updated(const Node& node,
                                                const cv::Rect& roi_hp,
                                                const cv::Size& hp_size,
                                                bool dirty_source,
                                                uint64_t dirty_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node);
  if (!is_rect_empty(roi_hp)) {
    entry.hp_roi = entry.hp_roi.has_value()
                       ? clip_rect(merge_rect(*entry.hp_roi, roi_hp), hp_size)
                       : roi_hp;
  }
  entry.hp_version++;
  if (dirty_source) {
    entry.dirty_source_generation = dirty_generation;
  }
  return entry.hp_version;
}

void HighPrecisionDirtyWriteBuffer::commit_to_graph(GraphModel& graph) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : entries_) {
    const int node_id = item.first;
    Entry& entry = item.second;
    if (!entry.has_output) {
      continue;
    }
    graph.mutate_node_runtime_state(
        node_id, [&](GraphModel::NodeRuntimeState& state) {
          state.cached_output_high_precision = std::move(entry.output);
          state.hp_roi = entry.hp_roi;
          state.hp_version = entry.hp_version;
        });
    if (entry.dirty_source_generation) {
      graph.dirty_source_hp_commit_generation[node_id] =
          *entry.dirty_source_generation;
    }
  }
}

std::vector<DownsampleExecutor::Request>
HighPrecisionDirtyWriteBuffer::downsample_requests() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DownsampleExecutor::Request> requests;
  for (const auto& [node_id, entry] : entries_) {
    if (!entry.has_output || !entry.hp_roi) {
      continue;
    }
    requests.push_back({node_id, *entry.hp_roi, entry.hp_version});
  }
  return requests;
}

HighPrecisionDirtyWriteBuffer::Entry&
HighPrecisionDirtyWriteBuffer::ensure_entry_locked(const Node& node) {
  Entry& entry = entries_[node.id];
  if (!entry.initialized) {
    entry.initialized = true;
    entry.hp_roi = node.hp_roi;
    entry.hp_version = node.hp_version;
    if (seed_existing_outputs_ && node.cached_output_high_precision) {
      entry.output =
          clone_node_output(*node.cached_output_high_precision, "HP");
      entry.has_output = true;
    }
  }
  return entry;
}

RealtimeProxyWriteBuffer::RealtimeProxyWriteBuffer(
    RealtimeProxyGraph& proxy_graph, bool seed_existing_outputs)
    : proxy_graph_(proxy_graph),
      seed_existing_outputs_(seed_existing_outputs) {}  // NOLINT

const NodeOutput* RealtimeProxyWriteBuffer::find_output(int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(node_id);
  if (it == entries_.end() || !it->second.has_output) {
    return nullptr;
  }
  return &*it->second.state.output;
}

bool RealtimeProxyWriteBuffer::has_output(int node_id) const {
  return find_output(node_id) != nullptr;
}

NodeOutput& RealtimeProxyWriteBuffer::ensure_output(int node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node_id);
  if (!entry.has_output) {
    entry.state.output = NodeOutput{};
    entry.has_output = true;
  }
  return *entry.state.output;
}

int RealtimeProxyWriteBuffer::mark_updated(int node_id, const cv::Rect& roi_hp,
                                           const cv::Size& hp_size,
                                           bool dirty_source,
                                           uint64_t dirty_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node_id);
  if (!is_rect_empty(roi_hp)) {
    entry.state.roi_hp =
        entry.state.roi_hp.has_value()
            ? clip_rect(merge_rect(*entry.state.roi_hp, roi_hp), hp_size)
            : roi_hp;
  }
  entry.state.version++;
  if (dirty_source) {
    entry.state.dirty_source_generation = dirty_generation;
  }
  return entry.state.version;
}

void RealtimeProxyWriteBuffer::commit_to_proxy_graph() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : entries_) {
    const int node_id = item.first;
    Entry& entry = item.second;
    if (!entry.has_output) {
      continue;
    }
    proxy_graph_.commit_node_state(node_id, std::move(entry.state));
  }
}

RealtimeProxyWriteBuffer::Entry& RealtimeProxyWriteBuffer::ensure_entry_locked(
    int node_id) {
  Entry& entry = entries_[node_id];
  if (!entry.initialized) {
    entry.initialized = true;
    if (const RealtimeProxyGraph::NodeState* state =
            proxy_graph_.find_state(node_id)) {
      entry.state.roi_hp = state->roi_hp;
      entry.state.version = state->version;
      entry.state.dirty_source_generation = state->dirty_source_generation;
      if (seed_existing_outputs_ && state->output) {
        entry.state.output = clone_node_output(*state->output, "RT proxy");
        entry.has_output = true;
      }
    }
  }
  return entry;
}

}  // namespace ps::compute
