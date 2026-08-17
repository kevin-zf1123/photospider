#include "compute/dirty/dirty_write_buffers.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/execution/resource_demand_estimator.hpp"
#include "core/value_image_adapter.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Copies immutable output state into request-local dirty staging.
 *
 * @param source Source output owned by graph or proxy state.
 * @param label Human-readable buffer domain used in error messages.
 * @return Independent metadata container retaining the same immutable Values.
 * @throws std::bad_alloc when output or metadata copying exhausts memory.
 * @throws GraphError when source still carries compatibility staging.
 * @note Named Values are immutable and safe to retain until a fresh binding is
 * allocated for an update. No mutable clone, second allocation authority, or
 * synthetic revision is created by this staging copy.
 */
NodeOutput copy_node_output_for_staging(const NodeOutput& source,
                                        const std::string& label) {
  if (source.has_compatibility_image()) {
    throw GraphError(GraphErrc::ComputeError,
                     label + " dirty staging rejects compatibility images.");
  }
  return source;
}

/**
 * @brief Compares every authoritative field of two frozen output plans.
 * @param left First complete plan.
 * @param right Second complete plan.
 * @return True only when name, logical metadata, layout, envelope, and
 * alignment are identical.
 * @throws Nothing under contained value equality contracts.
 * @note Derived dimensions and Region need no separate comparison because
 * DenseImageOutputPlan::create deterministically validates and derives them
 * from the compared authoritative fields.
 */
bool output_plans_match(const DenseImageOutputPlan& left,
                        const DenseImageOutputPlan& right) noexcept {
  return left.output_name() == right.output_name() &&
         left.descriptor() == right.descriptor() &&
         left.image_facet() == right.image_facet() &&
         left.layout() == right.layout() &&
         left.storage_size() == right.storage_size() &&
         left.alignment() == right.alignment();
}

/**
 * @brief Seeds one new binding when staged output exactly matches its plan.
 * @param output Immutable staged output selected as the copy-on-write base.
 * @param binding Fresh open binding.
 * @return True after all logical elements were copied; false when no exact
 * canonical image is available.
 * @throws ReadyFenceAccessError, BufferAccessError, std::out_of_range,
 * std::overflow_error, std::logic_error, or std::system_error from checked
 * Value access and grant lifecycle.
 * @note Metadata mismatch is decided before grant issuance and is not a
 * binding failure. Once seeding starts, any failure closes the binding.
 */
bool seed_tiled_binding(const NodeOutput& output, HostOutputBinding& binding) {
  if (!output.has_image_value()) {
    return false;
  }
  const Value& value = output.image_value();
  const DenseImageOutputPlan& plan = binding.plan();
  if (!(value.dense_tensor_descriptor() == plan.descriptor()) ||
      !value.image_facet().has_value() ||
      !(*value.image_facet() == plan.image_facet())) {
    return false;
  }
  binding.seed_from_value(value);
  return true;
}

/**
 * @brief Publishes one sealed binding Value into existing staged metadata.
 * @param output Mutable request-local output with no compatibility staging.
 * @param binding Sole drained binding to seal exactly once.
 * @return Nothing after canonical image replacement or first publication.
 * @throws std::invalid_argument, std::logic_error, std::overflow_error,
 * std::bad_alloc, or std::system_error from seal and NodeOutput publication.
 * @note The helper is called only after all physical tasks drain and before
 * graph/proxy visibility. It creates one Value revision and no cache revision.
 */
void seal_tiled_binding(NodeOutput* output, HostOutputBinding* binding) {
  if (output == nullptr || binding == nullptr ||
      output->has_compatibility_image()) {
    throw std::invalid_argument(
        "Dirty tiled seal requires canonical staged output and binding.");
  }
  Value value = binding->seal();
  if (output->has_image_value()) {
    output->replace_image_value(std::move(value));
  } else {
    output->publish_image_value(std::move(value));
  }
}

/**
 * @brief Validates one staged output before HP or RT publication.
 * @param output Request-local result selected for publication.
 * @return Nothing after every named Value is valid and Ready.
 * @throws GraphError when compatibility staging, an invalid name/Value, or a
 * non-Ready producer is observed.
 * @throws std::logic_error when a retained readiness observer is invalid.
 * @note Validation polls only immutable state and never imports a fallback,
 * maps storage, mints a revision, or mutates graph/proxy cache state.
 */
void validate_staged_output(const NodeOutput& output) {
  if (output.has_compatibility_image()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Dirty commit rejects compatibility image staging.");
  }
  for (const auto& [name, value] : output.named_values) {
    if (name.empty() || !value.valid() || !value.ready_fence().poll().ready()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Dirty commit requires valid Ready named Value outputs.");
    }
  }
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

/** @copydoc HighPrecisionDirtyWriteBuffer::copy_output */
std::optional<NodeOutput> HighPrecisionDirtyWriteBuffer::copy_output(
    int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(node_id);
  if (found == entries_.end() || !found->second.has_output) {
    return std::nullopt;
  }
  return found->second.output;
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

/** @copydoc HighPrecisionDirtyWriteBuffer::ensure_tiled_output_binding */
HostOutputBinding& HighPrecisionDirtyWriteBuffer::ensure_tiled_output_binding(
    const Node& node, DenseImageOutputPlan plan,
    std::size_t expected_task_count) {
  if (expected_task_count == 0U) {
    throw std::invalid_argument(
        "HP tiled binding requires a positive frozen task count.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node);
  if (entry.tiled_task_count != 0U) {
    if (entry.tiled_task_count != expected_task_count) {
      throw std::invalid_argument(
          "HP tiled tasks for one node require one frozen task count.");
    }
    if (!entry.tiled_binding.has_value()) {
      throw std::logic_error(
          "HP tiled node binding was already sealed or lost.");
    }
    if (!output_plans_match(entry.tiled_binding->plan(), plan)) {
      throw std::invalid_argument(
          "HP tiled tasks for one node require one frozen output plan.");
    }
    return *entry.tiled_binding;
  }

  HostOutputBinding binding = HostOutputBinding::allocate(std::move(plan));
  const bool preserved_existing_bytes =
      seed_tiled_binding(entry.output, binding);
  entry.tiled_binding.emplace(std::move(binding));
  entry.tiled_task_count = expected_task_count;
  entry.tiled_tasks_remaining = expected_task_count;
  entry.has_output = true;
  if (!preserved_existing_bytes) {
    entry.hp_region.reset();
  }
  return *entry.tiled_binding;
}

/** @copydoc HighPrecisionDirtyWriteBuffer::complete_tiled_task */
void HighPrecisionDirtyWriteBuffer::complete_tiled_task(const Node& node) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto entry_it = entries_.find(node.id);
  if (entry_it == entries_.end() ||
      !entry_it->second.tiled_binding.has_value() ||
      entry_it->second.tiled_task_count == 0U ||
      entry_it->second.tiled_tasks_remaining == 0U) {
    throw std::logic_error(
        "HP tiled task completion requires one active node binding.");
  }
  Entry& entry = entry_it->second;
  --entry.tiled_tasks_remaining;
  if (entry.tiled_tasks_remaining == 0U) {
    seal_tiled_binding(&entry.output, &*entry.tiled_binding);
    entry.tiled_binding.reset();
    entry.has_output = true;
  }
}

/** @copydoc HighPrecisionDirtyWriteBuffer::stage_region_output */
void HighPrecisionDirtyWriteBuffer::stage_region_output(
    const Node& node, NodeOutput output, const RegionSet& updated_region) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node);
  bool preserved_existing_validity = false;
  if (entry.has_output && entry.hp_region.has_value()) {
    const NodeOutput* merge_base = &entry.output;
    if (!entry.output.has_image_value() &&
        node.cached_output_high_precision.has_value() &&
        node.cached_output_high_precision->has_image_value() &&
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
    if (!entry.output.has_image_value() &&
        node.cached_output_high_precision.has_value() &&
        node.cached_output_high_precision->has_image_value() &&
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

void HighPrecisionDirtyWriteBuffer::commit_to_graph(
    GraphModel& graph, const std::vector<PlannedNodeWork>& planned_work) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : entries_) {
    const int node_id = item.first;
    Entry& entry = item.second;
    if (entry.tiled_binding.has_value() || entry.tiled_tasks_remaining != 0U) {
      throw std::logic_error("HP dirty commit observed undrained tiled node " +
                             std::to_string(node_id) + " (remaining " +
                             std::to_string(entry.tiled_tasks_remaining) +
                             " of " + std::to_string(entry.tiled_task_count) +
                             ").");
    }
    if (entry.has_output) {
      const auto authority =
          std::find_if(planned_work.begin(), planned_work.end(),
                       [node_id](const PlannedNodeWork& work) {
                         return work.node_id == node_id;
                       });
      if (authority == planned_work.end() ||
          !authority->output_authority.has_value()) {
        throw GraphError(GraphErrc::ComputeError,
                         "HP dirty commit lacks frozen output authority.");
      }
      validate_planned_output(entry.output, *authority->output_authority,
                              PlannedOutputReadiness::RequireReady);
    }
  }
  for (auto& item : entries_) {
    const int node_id = item.first;
    Entry& entry = item.second;
    if (!entry.has_output) {
      continue;
    }
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
    if (!entry.hp_region->is_empty() &&
        (entry.hp_region->is_whole() || entry.hp_region->atoms().size() != 1U ||
         !std::holds_alternative<ImageRect>(
             entry.hp_region->atoms().front()))) {
      continue;
    }
    requests.push_back({node_id, *entry.hp_region, entry.hp_version});
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
      entry.output = copy_node_output_for_staging(
          *node.cached_output_high_precision, "HP");
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

/** @copydoc RealtimeProxyWriteBuffer::copy_output */
std::optional<NodeOutput> RealtimeProxyWriteBuffer::copy_output(
    int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(node_id);
  if (found == entries_.end() || !found->second.has_output ||
      !found->second.state.output.has_value()) {
    return std::nullopt;
  }
  return *found->second.state.output;
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

/** @copydoc RealtimeProxyWriteBuffer::stage_output */
void RealtimeProxyWriteBuffer::stage_output(int node_id, NodeOutput output,
                                            bool preserved_existing_bytes) {
  validate_staged_output(output);
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node_id);
  if (entry.tiled_task_count != 0U) {
    throw std::logic_error(
        "RT node cannot stage monolithic output while a tiled binding exists.");
  }
  entry.state.output = std::move(output);
  entry.has_output = true;
  if (!preserved_existing_bytes) {
    entry.state.region_hp.reset();
  }
}

/** @copydoc RealtimeProxyWriteBuffer::ensure_tiled_output_binding */
HostOutputBinding& RealtimeProxyWriteBuffer::ensure_tiled_output_binding(
    int node_id, DenseImageOutputPlan plan, std::size_t expected_task_count) {
  if (expected_task_count == 0U) {
    throw std::invalid_argument(
        "RT tiled binding requires a positive frozen task count.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node_id);
  if (entry.tiled_task_count != 0U) {
    if (entry.tiled_task_count != expected_task_count) {
      throw std::invalid_argument(
          "RT tiled tasks for one node require one frozen task count.");
    }
    if (!entry.tiled_binding.has_value()) {
      throw std::logic_error(
          "RT tiled node binding was already sealed or lost.");
    }
    if (!output_plans_match(entry.tiled_binding->plan(), plan)) {
      throw std::invalid_argument(
          "RT tiled tasks for one node require one frozen output plan.");
    }
    return *entry.tiled_binding;
  }

  HostOutputBinding binding = HostOutputBinding::allocate(std::move(plan));
  const NodeOutput empty_output;
  const NodeOutput& seed_output =
      entry.state.output.has_value() ? *entry.state.output : empty_output;
  const bool preserved_existing_bytes =
      seed_tiled_binding(seed_output, binding);
  entry.tiled_binding.emplace(std::move(binding));
  entry.tiled_task_count = expected_task_count;
  entry.tiled_tasks_remaining = expected_task_count;
  if (!entry.state.output.has_value()) {
    entry.state.output.emplace();
  }
  entry.has_output = true;
  if (!preserved_existing_bytes) {
    entry.state.region_hp.reset();
  }
  return *entry.tiled_binding;
}

/** @copydoc RealtimeProxyWriteBuffer::complete_tiled_task */
void RealtimeProxyWriteBuffer::complete_tiled_task(int node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto entry_it = entries_.find(node_id);
  if (entry_it == entries_.end() ||
      !entry_it->second.tiled_binding.has_value() ||
      entry_it->second.tiled_task_count == 0U ||
      entry_it->second.tiled_tasks_remaining == 0U) {
    throw std::logic_error(
        "RT tiled task completion requires one active node binding.");
  }
  Entry& entry = entry_it->second;
  --entry.tiled_tasks_remaining;
  if (entry.tiled_tasks_remaining == 0U) {
    if (!entry.state.output.has_value()) {
      entry.state.output.emplace();
    }
    seal_tiled_binding(&*entry.state.output, &*entry.tiled_binding);
    entry.tiled_binding.reset();
    entry.has_output = true;
  }
}

/** @copydoc RealtimeProxyWriteBuffer::mark_updated */
int RealtimeProxyWriteBuffer::mark_updated(int node_id,
                                           const RegionSet& region_hp,
                                           bool dirty_source,
                                           uint64_t dirty_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = ensure_entry_locked(node_id);
  if (!region_hp.is_empty()) {
    if (region_hp.is_whole() || region_hp.atoms().size() != 1U ||
        !std::holds_alternative<ImageRect>(region_hp.atoms().front()) ||
        !(std::get<ImageRect>(region_hp.atoms().front()).domain ==
          image_region_domain())) {
      throw std::invalid_argument(
          "RT validity metadata requires one exact image Region.");
    }
    entry.state.region_hp =
        merge_valid_regions(entry.state.region_hp, region_hp);
  }
  entry.state.version++;
  if (dirty_source) {
    entry.state.dirty_source_generation = dirty_generation;
  }
  return entry.state.version;
}

void RealtimeProxyWriteBuffer::commit_to_proxy_graph(
    const std::vector<PlannedNodeWork>& planned_work) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : entries_) {
    const int node_id = item.first;
    Entry& entry = item.second;
    if (entry.tiled_binding.has_value() || entry.tiled_tasks_remaining != 0U) {
      throw std::logic_error("RT dirty commit observed undrained tiled node " +
                             std::to_string(node_id) + " (remaining " +
                             std::to_string(entry.tiled_tasks_remaining) +
                             " of " + std::to_string(entry.tiled_task_count) +
                             ").");
    }
    if (entry.has_output && entry.state.output.has_value()) {
      const auto authority =
          std::find_if(planned_work.begin(), planned_work.end(),
                       [node_id](const PlannedNodeWork& work) {
                         return work.node_id == node_id;
                       });
      if (authority == planned_work.end() ||
          !authority->output_authority.has_value()) {
        throw GraphError(GraphErrc::ComputeError,
                         "RT dirty commit lacks frozen output authority.");
      }
      validate_planned_output(*entry.state.output, *authority->output_authority,
                              PlannedOutputReadiness::RequireReady);
    }
  }
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
        entry.state.output =
            copy_node_output_for_staging(*state->output, "RT proxy");
        entry.has_output = true;
      }
    }
  }
  return entry;
}

}  // namespace ps::compute
