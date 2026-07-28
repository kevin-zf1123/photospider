#include "compute/dirty_write_buffers.hpp"

#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "core/image_buffer_processing.hpp"
#include "core/region_image_adapter.hpp"
#include "core/value_image_adapter.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Deep-copies an image buffer for staged dirty writes.
 *
 * @param source Source image buffer owned by graph or proxy state.
 * @param label Human-readable buffer domain used in error messages.
 * @return Independent CPU ImageBuffer when CPU pixels are present; otherwise a
 * shared backend descriptor whose immutable owner is safe to replace later.
 * @throws GraphError when neutral CPU-buffer cloning fails; may throw
 * std::bad_alloc while allocating independent storage.
 * @note Empty buffers keep shape metadata but drop ownership. Non-CPU buffers
 * are shallow-copied because the generic host cannot clone opaque resources;
 * tiled execution replaces that descriptor with a CPU staging allocation
 * before writing, while monolithic execution replaces the complete output.
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
  if (source.device != Device::CPU) {
    return source;
  }

  try {
    cloned = image_processing::clone_cpu_image_buffer(source);
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
 * @return Independent output with cloned image payload, cleared image Value
 * identity, and copied non-image metadata.
 * @throws std::bad_alloc when output or metadata copying exhausts memory.
 * @throws GraphError when image payload cloning otherwise fails.
 * @note Named ParameterValue data, spatial context, and debug metadata are
 * value-copied. Clearing `image_value` prevents mutable staged bytes from
 * retaining the source cache revision; HP commit reseals final bytes.
 */
NodeOutput clone_node_output(const NodeOutput& source,
                             const std::string& label) {
  NodeOutput cloned = source;
  cloned.image_buffer = clone_image_buffer(source.image_buffer, label);
  cloned.image_value = Value{};
  return cloned;
}

/**
 * @brief Merges validity Regions without inventing unverified coordinates.
 *
 * @param existing Previously staged exact validity, when any.
 * @param update Newly validated exact update Region.
 * @return Exact representable union; when the bounded subset cannot represent
 *         both, returns `update` as a safe under-approximation.
 * @throws std::bad_alloc when Region algebra storage cannot allocate.
 * @note Validity metadata must never use a conservative superset because that
 *       could authorize reuse of bytes not proven current.
 */
RegionSet merge_valid_regions(const std::optional<RegionSet>& existing,
                              const RegionSet& update) {
  if (!existing.has_value()) {
    return update;
  }
  const RegionOperationResult merged = union_regions(*existing, update);
  if (merged.status() == RegionOperationStatus::Exact &&
      merged.region().has_value()) {
    return *merged.region();
  }
  return update;
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

/** @copydoc HighPrecisionDirtyWriteBuffer::stage_region_output */
void HighPrecisionDirtyWriteBuffer::stage_region_output(
    const Node& node, NodeOutput output, const RegionSet& updated_region) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node);
  bool preserved_existing_validity = false;
  if (entry.has_output && entry.hp_region.has_value()) {
    const NodeOutput* merge_base = &entry.output;
    if (!entry.output.image_value.valid() &&
        node.cached_output_high_precision.has_value() &&
        node.cached_output_high_precision->image_value.valid() &&
        entry.hp_version == node.hp_version) {
      merge_base = &*node.cached_output_high_precision;
    }
    std::optional<NodeOutput> merged =
        value_image_adapter::merge_node_output_region(*merge_base, output,
                                                      updated_region);
    if (merged.has_value()) {
      entry.output = std::move(*merged);
      preserved_existing_validity = true;
    }
  }
  if (!preserved_existing_validity) {
    entry.output = std::move(output);
    entry.hp_region.reset();
  }
  entry.has_output = true;
}

/** @copydoc HighPrecisionDirtyWriteBuffer::import_precomputed_output */
void HighPrecisionDirtyWriteBuffer::import_precomputed_output(
    const Node& node, const NodeOutput& output, int hp_version,
    const std::optional<RegionSet>& hp_region,
    std::optional<uint64_t> dirty_source_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node);
  entry.output = output;
  entry.has_output = true;
  entry.hp_version = hp_version;
  entry.hp_region = hp_region;
  entry.dirty_source_generation = dirty_source_generation;
}

/** @copydoc HighPrecisionDirtyWriteBuffer::mark_updated */
int HighPrecisionDirtyWriteBuffer::mark_updated(const Node& node,
                                                const RegionSet& region_hp,
                                                bool dirty_source,
                                                uint64_t dirty_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node);
  if (!region_hp.is_empty()) {
    const NodeOutput* validity_output = &entry.output;
    if (!entry.output.image_value.valid() &&
        node.cached_output_high_precision.has_value() &&
        node.cached_output_high_precision->image_value.valid() &&
        entry.hp_version == node.hp_version) {
      validity_output = &*node.cached_output_high_precision;
    }
    const bool already_complete =
        entry.has_output && entry.hp_region.has_value() &&
        value_image_adapter::node_output_region_is_complete(*validity_output,
                                                            *entry.hp_region);
    if (!already_complete) {
      entry.hp_region = merge_valid_regions(entry.hp_region, region_hp);
    }
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
    value_image_adapter::normalize_node_output_image_value(&entry.output);
    graph.mutate_node_runtime_state(
        node_id, [&](GraphModel::NodeRuntimeState& state) {
          state.cached_output_high_precision = std::move(entry.output);
          state.hp_region = entry.hp_region;
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
    if (!entry.has_output || !entry.hp_region) {
      continue;
    }
    try {
      requests.push_back({node_id,
                          region_image_adapter::to_pixel_rect(*entry.hp_region),
                          entry.hp_version});
    } catch (const std::invalid_argument&) {
      // TensorSlice and Whole have no current image-only downsample projection.
    }
  }
  return requests;
}

/** @copydoc HighPrecisionDirtyWriteBuffer::retained_memory_bytes */
std::uint64_t HighPrecisionDirtyWriteBuffer::retained_memory_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  RetainedMemoryEstimator estimate("HighPrecisionDirtyWriteBuffer");
  estimate.add_objects<HighPrecisionDirtyWriteBuffer>();
  estimate.add_objects<decltype(entries_)::value_type>(
      static_cast<std::uint64_t>(entries_.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(entries_.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(entries_.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(entries_.size()));
  for (const auto& [node_id, entry] : entries_) {
    (void)node_id;
    if (entry.hp_region.has_value()) {
      estimate.add_bytes(
          region_dynamic_retained_memory_bytes(*entry.hp_region));
    }
    if (entry.has_output) {
      estimate.add_bytes(
          node_output_dynamic_retained_memory_bytes(entry.output));
    }
  }
  return estimate.bytes();
}

/** @copydoc HighPrecisionDirtyWriteBuffer::missing_entry_retained_memory_bytes
 */
std::uint64_t
HighPrecisionDirtyWriteBuffer::missing_entry_retained_memory_bytes(
    const GraphModel& graph,
    const std::vector<int>& anticipated_node_ids) const {
  std::lock_guard<std::mutex> lock(mutex_);
  RetainedMemoryEstimator estimate(
      "HighPrecisionDirtyWriteBuffer pending entries");
  const NodeOutput empty_output;
  std::unordered_set<int> unique_node_ids;
  unique_node_ids.reserve(anticipated_node_ids.size());
  for (int node_id : anticipated_node_ids) {
    if (!unique_node_ids.insert(node_id).second ||
        entries_.find(node_id) != entries_.end()) {
      continue;
    }
    estimate.add_objects<decltype(entries_)::value_type>();
    estimate.add_objects<void*>(3U);
    const Node& node = graph.node(node_id);
    const bool seed_existing_output =
        seed_existing_outputs_ && node.cached_output_high_precision.has_value();
    const NodeOutput* seeded_output = seed_existing_output
                                          ? &*node.cached_output_high_precision
                                          : &empty_output;
    estimate.add_bytes(
        node_output_dynamic_retained_memory_bytes(*seeded_output));
    if (seed_existing_output && node.hp_region.has_value()) {
      estimate.add_bytes(region_dynamic_retained_memory_bytes(*node.hp_region));
    }
  }
  return estimate.bytes();
}

HighPrecisionDirtyWriteBuffer::Entry&
HighPrecisionDirtyWriteBuffer::ensure_entry_locked(const Node& node) {
  Entry& entry = entries_[node.id];
  if (!entry.initialized) {
    entry.initialized = true;
    entry.hp_version = node.hp_version;
    if (seed_existing_outputs_ && node.cached_output_high_precision) {
      entry.hp_region = node.hp_region;
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

/** @copydoc RealtimeProxyWriteBuffer::mark_updated */
int RealtimeProxyWriteBuffer::mark_updated(int node_id,
                                           const RegionSet& region_hp,
                                           bool dirty_source,
                                           uint64_t dirty_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node_id);
  if (!region_hp.is_empty()) {
    (void)region_image_adapter::to_pixel_rect(region_hp);
    entry.state.region_hp =
        merge_valid_regions(entry.state.region_hp, region_hp);
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

/** @copydoc RealtimeProxyWriteBuffer::retained_memory_bytes */
std::uint64_t RealtimeProxyWriteBuffer::retained_memory_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  RetainedMemoryEstimator estimate("RealtimeProxyWriteBuffer");
  estimate.add_objects<RealtimeProxyWriteBuffer>();
  estimate.add_objects<decltype(entries_)::value_type>(
      static_cast<std::uint64_t>(entries_.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(entries_.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(entries_.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(entries_.size()));
  for (const auto& [node_id, entry] : entries_) {
    (void)node_id;
    if (entry.state.region_hp.has_value()) {
      estimate.add_bytes(
          region_dynamic_retained_memory_bytes(*entry.state.region_hp));
    }
    if (entry.has_output && entry.state.output.has_value()) {
      estimate.add_bytes(
          node_output_dynamic_retained_memory_bytes(*entry.state.output));
    }
  }
  return estimate.bytes();
}

/** @copydoc RealtimeProxyWriteBuffer::missing_entry_retained_memory_bytes */
std::uint64_t RealtimeProxyWriteBuffer::missing_entry_retained_memory_bytes(
    const std::vector<int>& anticipated_node_ids) const {
  std::lock_guard<std::mutex> lock(mutex_);
  RetainedMemoryEstimator estimate("RealtimeProxyWriteBuffer pending entries");
  const NodeOutput empty_output;
  std::unordered_set<int> unique_node_ids;
  unique_node_ids.reserve(anticipated_node_ids.size());
  for (int node_id : anticipated_node_ids) {
    if (!unique_node_ids.insert(node_id).second ||
        entries_.find(node_id) != entries_.end()) {
      continue;
    }
    estimate.add_objects<decltype(entries_)::value_type>();
    estimate.add_objects<void*>(3U);
    const RealtimeProxyGraph::NodeState* state =
        proxy_graph_.find_state(node_id);
    const bool seed_existing_output =
        seed_existing_outputs_ && state && state->output.has_value();
    const NodeOutput* seeded_output =
        seed_existing_output ? &*state->output : &empty_output;
    estimate.add_bytes(
        node_output_dynamic_retained_memory_bytes(*seeded_output));
    if (seed_existing_output && state->region_hp.has_value()) {
      estimate.add_bytes(
          region_dynamic_retained_memory_bytes(*state->region_hp));
    }
  }
  return estimate.bytes();
}

RealtimeProxyWriteBuffer::Entry& RealtimeProxyWriteBuffer::ensure_entry_locked(
    int node_id) {
  Entry& entry = entries_[node_id];
  if (!entry.initialized) {
    entry.initialized = true;
    if (const RealtimeProxyGraph::NodeState* state =
            proxy_graph_.find_state(node_id)) {
      entry.state.version = state->version;
      entry.state.dirty_source_generation = state->dirty_source_generation;
      if (seed_existing_outputs_ && state->output) {
        entry.state.region_hp = state->region_hp;
        entry.state.output = clone_node_output(*state->output, "RT proxy");
        entry.has_output = true;
      }
    }
  }
  return entry;
}

}  // namespace ps::compute
