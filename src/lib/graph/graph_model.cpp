#include "graph/graph_model.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps {

namespace {

using NodeMap = GraphModel::NodeMap;

const Node& require_node(const NodeMap& nodes, int id) {
  auto it = nodes.find(id);
  if (it == nodes.end()) {
    throw GraphError(GraphErrc::NotFound,
                     "Node " + std::to_string(id) + " not in graph.");
  }
  return it->second;
}

GraphTopologyIndex build_topology_index(const NodeMap& nodes) {
  GraphTopologyIndex index;
  for (const auto& [id, _] : nodes) {
    index.incoming_by_node.emplace(id, std::vector<GraphTopologyEdge>{});
    index.outgoing_by_node.emplace(id, std::vector<GraphTopologyEdge>{});
  }

  for (const auto& [to_id, node] : nodes) {
    for (size_t i = 0; i < node.image_inputs.size(); ++i) {
      const auto& input = node.image_inputs[i];
      if (input.from_node_id < 0) {
        continue;
      }
      GraphTopologyEdge edge;
      edge.from_node_id = input.from_node_id;
      edge.to_node_id = to_id;
      edge.kind = GraphTopologyEdgeKind::ImageInput;
      edge.from_output_name = input.from_output_name;
      edge.to_input_name = "image";
      edge.input_index = i;
      index.incoming_by_node[to_id].push_back(edge);
      index.outgoing_by_node[input.from_node_id].push_back(edge);
    }
    for (size_t i = 0; i < node.parameter_inputs.size(); ++i) {
      const auto& input = node.parameter_inputs[i];
      if (input.from_node_id < 0) {
        continue;
      }
      GraphTopologyEdge edge;
      edge.from_node_id = input.from_node_id;
      edge.to_node_id = to_id;
      edge.kind = GraphTopologyEdgeKind::ParameterInput;
      edge.from_output_name = input.from_output_name;
      edge.to_input_name = input.to_parameter_name;
      edge.input_index = i;
      index.incoming_by_node[to_id].push_back(edge);
      index.outgoing_by_node[input.from_node_id].push_back(edge);
    }
  }
  return index;
}

bool is_ancestor(const NodeMap& nodes, int potential_ancestor_id, int node_id,
                 std::unordered_set<int>& visited) {
  if (potential_ancestor_id == node_id) {
    return true;
  }
  if (visited.count(node_id)) {
    return false;
  }
  visited.insert(node_id);

  auto it = nodes.find(node_id);
  if (it == nodes.end()) {
    return false;
  }
  const Node& node = it->second;
  for (const auto& input : node.image_inputs) {
    if (input.from_node_id != -1 && is_ancestor(nodes, potential_ancestor_id,
                                                input.from_node_id, visited)) {
      return true;
    }
  }
  for (const auto& input : node.parameter_inputs) {
    if (input.from_node_id != -1 && is_ancestor(nodes, potential_ancestor_id,
                                                input.from_node_id, visited)) {
      return true;
    }
  }
  return false;
}

void validate_node_dfs(const NodeMap& nodes, int node_id,
                       std::unordered_set<int>& visiting,
                       std::unordered_set<int>& visited) {
  if (visited.count(node_id)) {
    return;
  }
  if (visiting.count(node_id)) {
    throw GraphError(GraphErrc::Cycle,
                     "Cycle detected while validating graph topology.");
  }

  auto node_it = nodes.find(node_id);
  if (node_it == nodes.end()) {
    throw GraphError(GraphErrc::MissingDependency,
                     "Missing node " + std::to_string(node_id) +
                         " while validating graph topology.");
  }

  visiting.insert(node_id);
  const Node& node = node_it->second;
  auto visit_dependency = [&](int dependency_id) {
    if (dependency_id == -1) {
      return;
    }
    if (nodes.find(dependency_id) == nodes.end()) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(node.id) +
                           " references missing node " +
                           std::to_string(dependency_id) + ".");
    }
    validate_node_dfs(nodes, dependency_id, visiting, visited);
  };

  for (const auto& input : node.image_inputs) {
    visit_dependency(input.from_node_id);
  }
  for (const auto& input : node.parameter_inputs) {
    visit_dependency(input.from_node_id);
  }
  visiting.erase(node_id);
  visited.insert(node_id);
}

void validate_node_map(const NodeMap& nodes) {
  std::unordered_set<int> visiting;
  std::unordered_set<int> visited;
  for (const auto& [id, _] : nodes) {
    validate_node_dfs(nodes, id, visiting, visited);
  }
}

/**
 * @brief Computes the next topology generation without unsigned wrapping.
 * @param generation Current topology-only cache generation.
 * @return Exact successor generation.
 * @throws std::overflow_error when generation is UINT64_MAX.
 * @note This counter remains separate from authoritative GraphRevision.
 */
uint64_t next_topology_generation(uint64_t generation) {
  if (generation == std::numeric_limits<uint64_t>::max()) {
    throw std::overflow_error("Graph topology generation is exhausted.");
  }
  return generation + 1;
}

}  // namespace

void GraphTopologyIndex::clear() {
  incoming_by_node.clear();
  outgoing_by_node.clear();
}

bool GraphTopologyIndex::empty() const {
  return incoming_by_node.empty() && outgoing_by_node.empty();
}

/** @copydoc GraphModel::GraphModel(fs::path) */
GraphModel::GraphModel(fs::path cache_root_dir)
    : cache_root(std::move(cache_root_dir)),
      instance_id_(GraphInstanceId::mint()),
      revision_(GraphRevision::initial()) {
  if (!cache_root.empty()) {
    fs::create_directories(cache_root);
  }
}

/** @copydoc
 * GraphModel::GraphModel(ComputeSnapshotTag,fs::path,GraphInstanceId,GraphRevision)
 */
GraphModel::GraphModel(ComputeSnapshotTag tag, fs::path cache_root_dir,
                       GraphInstanceId instance_id, GraphRevision revision)
    : cache_root(std::move(cache_root_dir)),
      instance_id_(instance_id),
      revision_(revision),
      compute_snapshot_(true) {
  (void)tag;
}

/** @copydoc GraphModel::clone_for_compute */
std::unique_ptr<GraphModel> GraphModel::clone_for_compute() const {
  auto snapshot = std::unique_ptr<GraphModel>(new GraphModel(
      ComputeSnapshotTag{}, cache_root, instance_id_, revision_));
  snapshot->timing_results = timing_results;
  snapshot->last_dirty_region_snapshot_debug = last_dirty_region_snapshot_debug;
  snapshot->last_dirty_region_snapshot = last_dirty_region_snapshot;
  snapshot->recent_dirty_region_snapshots = recent_dirty_region_snapshots;
  snapshot->dirty_generation_counter = dirty_generation_counter;
  snapshot->dirty_source_hp_commit_generation =
      dirty_source_hp_commit_generation;
  snapshot->last_compute_plan = last_compute_plan;
  snapshot->recent_compute_plans = recent_compute_plans;
  snapshot->last_compute_plan_summary = last_compute_plan_summary;
  snapshot->recent_compute_plan_summaries = recent_compute_plan_summaries;
  snapshot->nodes_ = nodes_;
  snapshot->topology_ = topology_;
  snapshot->last_disk_cache_load_result_ =
      last_disk_cache_load_result_snapshot();
  snapshot->topology_generation_ = topology_generation_;
  snapshot->full_task_graph_cache_ = full_task_graph_cache_;
  snapshot->quiet_ = quiet_;
  snapshot->skip_save_cache_.store(skip_save_cache(),
                                   std::memory_order_relaxed);
  snapshot->total_io_time_ms.store(
      total_io_time_ms.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  return snapshot;
}

/** @copydoc GraphModel::publish_compute_snapshot */
void GraphModel::publish_compute_snapshot(GraphModel& prepared) noexcept {
  static_assert(std::is_nothrow_swappable_v<decltype(timing_results)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(last_dirty_region_snapshot_debug)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(last_dirty_region_snapshot)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(recent_dirty_region_snapshots)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(dirty_generation_counter)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(dirty_source_hp_commit_generation)>);
  static_assert(std::is_nothrow_swappable_v<decltype(last_compute_plan)>);
  static_assert(std::is_nothrow_swappable_v<decltype(recent_compute_plans)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(last_compute_plan_summary)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(recent_compute_plan_summaries)>);
  static_assert(std::is_nothrow_swappable_v<decltype(nodes_)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(topology_.incoming_by_node)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(topology_.outgoing_by_node)>);
  static_assert(
      std::is_nothrow_swappable_v<decltype(last_disk_cache_load_result_)>);
  static_assert(std::is_nothrow_swappable_v<decltype(full_task_graph_cache_)>);
  using std::swap;
  swap(timing_results, prepared.timing_results);
  swap(last_dirty_region_snapshot_debug,
       prepared.last_dirty_region_snapshot_debug);
  swap(last_dirty_region_snapshot, prepared.last_dirty_region_snapshot);
  swap(recent_dirty_region_snapshots, prepared.recent_dirty_region_snapshots);
  swap(dirty_generation_counter, prepared.dirty_generation_counter);
  dirty_source_hp_commit_generation.swap(
      prepared.dirty_source_hp_commit_generation);
  swap(last_compute_plan, prepared.last_compute_plan);
  recent_compute_plans.swap(prepared.recent_compute_plans);
  swap(last_compute_plan_summary, prepared.last_compute_plan_summary);
  recent_compute_plan_summaries.swap(prepared.recent_compute_plan_summaries);
  nodes_.swap(prepared.nodes_);
  topology_.incoming_by_node.swap(prepared.topology_.incoming_by_node);
  topology_.outgoing_by_node.swap(prepared.topology_.outgoing_by_node);
  last_disk_cache_load_result_.swap(prepared.last_disk_cache_load_result_);
  full_task_graph_cache_.swap(prepared.full_task_graph_cache_);

  const double live_io = total_io_time_ms.load(std::memory_order_relaxed);
  total_io_time_ms.store(
      prepared.total_io_time_ms.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  prepared.total_io_time_ms.store(live_io, std::memory_order_relaxed);
}

void GraphModel::set_quiet(bool q) {
  quiet_ = q;
}

bool GraphModel::is_quiet() const {
  return quiet_;
}

void GraphModel::record_disk_cache_load_result(DiskCacheLoadResult result) {
  std::lock_guard<std::mutex> lock(disk_cache_diagnostics_mutex_);
  last_disk_cache_load_result_ = std::move(result);
}

std::optional<GraphModel::DiskCacheLoadResult>
GraphModel::last_disk_cache_load_result_snapshot() const {
  std::lock_guard<std::mutex> lock(disk_cache_diagnostics_mutex_);
  return last_disk_cache_load_result_;
}

void GraphModel::clear_disk_cache_load_result() {
  std::lock_guard<std::mutex> lock(disk_cache_diagnostics_mutex_);
  last_disk_cache_load_result_.reset();
}

/** @copydoc GraphModel::reset_runtime_state */
void GraphModel::reset_runtime_state() noexcept {
  timing_results.node_timings.clear();
  timing_results.total_ms = 0.0;
  last_dirty_region_snapshot_debug.reset();
  last_dirty_region_snapshot.reset();
  recent_dirty_region_snapshots.clear();
  dirty_generation_counter = 0;
  dirty_source_hp_commit_generation.clear();
  last_compute_plan.reset();
  recent_compute_plans.clear();
  last_compute_plan_summary.reset();
  recent_compute_plan_summaries.clear();
  full_task_graph_cache_.clear();
  last_disk_cache_load_result_.reset();
  total_io_time_ms.store(0.0, std::memory_order_relaxed);
  skip_save_cache_.store(false, std::memory_order_relaxed);
}

/** @copydoc GraphModel::clear */
void GraphModel::clear() {
  const GraphRevision next_revision = revision_.next();
  const uint64_t next_generation =
      next_topology_generation(topology_generation_);
  nodes_.clear();
  topology_.clear();
  reset_runtime_state();
  topology_generation_ = next_generation;
  revision_ = next_revision;
  quiet_ = true;
}

/** @copydoc GraphModel::add_node */
void GraphModel::add_node(const Node& node) {
  if (has_node(node.id)) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "Node with id " + std::to_string(node.id) + " already exists.");
  }

  std::unordered_set<int> potential_inputs;
  for (const auto& input : node.image_inputs) {
    potential_inputs.insert(input.from_node_id);
  }
  for (const auto& input : node.parameter_inputs) {
    potential_inputs.insert(input.from_node_id);
  }
  for (int input_id : potential_inputs) {
    if (input_id != -1) {
      std::unordered_set<int> visited;
      if (is_ancestor(nodes_, node.id, input_id, visited)) {
        throw GraphError(
            GraphErrc::Cycle,
            "Adding node " + std::to_string(node.id) + " creates a cycle.");
      }
    }
  }

  NodeMap candidate = nodes_;
  candidate[node.id] = node;
  validate_node_map(candidate);
  GraphTopologyIndex topology = build_topology_index(candidate);
  publish_structural_replacement(std::move(candidate), std::move(topology),
                                 false);
}

/** @copydoc GraphModel::replace_node */
void GraphModel::replace_node(const Node& node) {
  if (!has_node(node.id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Node " + std::to_string(node.id) + " not in graph.");
  }
  NodeMap candidate = nodes_;
  candidate[node.id] = node;
  validate_node_map(candidate);
  GraphTopologyIndex topology = build_topology_index(candidate);
  publish_structural_replacement(std::move(candidate), std::move(topology),
                                 false);
}

/** @copydoc GraphModel::remove_node */
void GraphModel::remove_node(int id) {
  if (!has_node(id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Node " + std::to_string(id) + " not in graph.");
  }
  NodeMap candidate = nodes_;
  candidate.erase(id);
  for (auto& [_, node] : candidate) {
    for (auto& input : node.image_inputs) {
      if (input.from_node_id == id) {
        input.from_node_id = -1;
      }
    }
    for (auto& input : node.parameter_inputs) {
      if (input.from_node_id == id) {
        input.from_node_id = -1;
      }
    }
  }
  validate_node_map(candidate);
  GraphTopologyIndex topology = build_topology_index(candidate);
  publish_structural_replacement(std::move(candidate), std::move(topology),
                                 false);
}

/** @copydoc GraphModel::rewire_image_input */
void GraphModel::rewire_image_input(int node_id, size_t input_index,
                                    int from_node_id,
                                    std::string from_output_name) {
  NodeMap candidate = nodes_;
  Node& target = const_cast<Node&>(require_node(candidate, node_id));
  if (input_index >= target.image_inputs.size()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Image input index out of range for node " +
                         std::to_string(node_id) + ".");
  }
  target.image_inputs[input_index].from_node_id = from_node_id;
  target.image_inputs[input_index].from_output_name =
      std::move(from_output_name);
  validate_node_map(candidate);
  GraphTopologyIndex topology = build_topology_index(candidate);
  publish_structural_replacement(std::move(candidate), std::move(topology),
                                 false);
}

/** @copydoc GraphModel::rewire_parameter_input */
void GraphModel::rewire_parameter_input(int node_id, size_t input_index,
                                        int from_node_id,
                                        std::string from_output_name,
                                        std::string to_parameter_name) {
  NodeMap candidate = nodes_;
  Node& target = const_cast<Node&>(require_node(candidate, node_id));
  if (input_index >= target.parameter_inputs.size()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Parameter input index out of range for node " +
                         std::to_string(node_id) + ".");
  }
  target.parameter_inputs[input_index].from_node_id = from_node_id;
  target.parameter_inputs[input_index].from_output_name =
      std::move(from_output_name);
  target.parameter_inputs[input_index].to_parameter_name =
      std::move(to_parameter_name);
  validate_node_map(candidate);
  GraphTopologyIndex topology = build_topology_index(candidate);
  publish_structural_replacement(std::move(candidate), std::move(topology),
                                 false);
}

/** @copydoc GraphModel::replace_nodes */
void GraphModel::replace_nodes(NodeMap nodes) {
  validate_node_map(nodes);
  GraphTopologyIndex topology = build_topology_index(nodes);
  publish_structural_replacement(std::move(nodes), std::move(topology), true);
}

/** @copydoc GraphModel::publish_structural_replacement */
void GraphModel::publish_structural_replacement(NodeMap nodes,
                                                GraphTopologyIndex topology,
                                                bool reset_runtime) {
  const GraphRevision next_revision = revision_.next();
  const uint64_t next_generation =
      next_topology_generation(topology_generation_);
  nodes_.swap(nodes);
  topology_.incoming_by_node.swap(topology.incoming_by_node);
  topology_.outgoing_by_node.swap(topology.outgoing_by_node);
  if (reset_runtime) {
    reset_runtime_state();
  } else {
    full_task_graph_cache_.clear();
  }
  topology_generation_ = next_generation;
  revision_ = next_revision;
}

bool GraphModel::has_node(int id) const {
  return nodes_.count(id) > 0;
}

bool GraphModel::empty() const {
  return nodes_.empty();
}

size_t GraphModel::node_count() const {
  return nodes_.size();
}

std::vector<int> GraphModel::node_ids() const {
  std::vector<int> ids;
  ids.reserve(nodes_.size());
  for (const auto& [id, _] : nodes_) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

const Node* GraphModel::find_node(int id) const {
  auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return nullptr;
  }
  return &it->second;
}

Node* GraphModel::find_node_mutable(int id) {
  auto it = nodes_.find(id);
  if (it == nodes_.end()) {
    return nullptr;
  }
  return &it->second;
}

const Node& GraphModel::node(int id) const {
  return require_node(nodes_, id);
}

Node& GraphModel::mutable_node(int id) {
  return const_cast<Node&>(require_node(nodes_, id));
}

void GraphModel::mutate_node_runtime_state(
    int id, const std::function<void(NodeRuntimeState&)>& mutator) {
  Node& node = mutable_node(id);
  NodeRuntimeState state{
      node.runtime_parameters,
      node.outputs,
      node.caches,
      node.preserved,
      node.cached_output_high_precision,
      node.hp_version,
      node.hp_roi,
      node.last_input_size_hp,
      node.dependency_lut_cache,
      node.dependency_lut_version,
      node.parameters_version,
  };
  mutator(state);
}

void GraphModel::for_each_node(
    const std::function<void(const Node&)>& visitor) const {
  for (int id : node_ids()) {
    visitor(node(id));
  }
}

void GraphModel::validate_topology() const {
  validate_node_map(nodes_);
}

/** @copydoc GraphModel::rebuild_topology_index */
void GraphModel::rebuild_topology_index() {
  validate_node_map(nodes_);
  GraphTopologyIndex topology = build_topology_index(nodes_);
  const GraphRevision next_revision = revision_.next();
  const uint64_t next_generation =
      next_topology_generation(topology_generation_);
  topology_.incoming_by_node.swap(topology.incoming_by_node);
  topology_.outgoing_by_node.swap(topology.outgoing_by_node);
  topology_generation_ = next_generation;
  revision_ = next_revision;
  full_task_graph_cache_.clear();
}

const GraphTopologyIndex& GraphModel::topology() const {
  return topology_;
}

const std::vector<GraphTopologyEdge>& GraphModel::upstream_edges(
    int node_id) const {
  static const std::vector<GraphTopologyEdge> empty_edges;
  auto it = topology_.incoming_by_node.find(node_id);
  if (it == topology_.incoming_by_node.end()) {
    return empty_edges;
  }
  return it->second;
}

const std::vector<GraphTopologyEdge>& GraphModel::downstream_edges(
    int node_id) const {
  static const std::vector<GraphTopologyEdge> empty_edges;
  auto it = topology_.outgoing_by_node.find(node_id);
  if (it == topology_.outgoing_by_node.end()) {
    return empty_edges;
  }
  return it->second;
}

uint64_t GraphModel::topology_generation() const {
  return topology_generation_;
}

std::shared_ptr<const compute::FullTaskGraph>
GraphModel::cached_full_task_graph(const std::string& key) const {
  auto it = full_task_graph_cache_.find(key);
  if (it == full_task_graph_cache_.end()) {
    return nullptr;
  }
  return it->second;
}

void GraphModel::remember_full_task_graph(
    const std::string& key,
    std::shared_ptr<const compute::FullTaskGraph> graph) {
  if (!graph) {
    return;
  }
  full_task_graph_cache_[key] = std::move(graph);
}

void GraphModel::clear_full_task_graph_cache() {
  full_task_graph_cache_.clear();
}

void GraphModel::set_skip_save_cache(bool v) {
  skip_save_cache_.store(v, std::memory_order_relaxed);
}

bool GraphModel::skip_save_cache() const {
  return skip_save_cache_.load(std::memory_order_relaxed);
}

plugin::ParameterMap resolve_effective_parameter_snapshot(
    const Node& node, const GraphModel& graph) {
  plugin::ParameterMap effective = node.parameters;
  for (const auto& input : node.parameter_inputs) {
    if (input.from_node_id < 0) {
      continue;
    }
    if (!graph.has_node(input.from_node_id)) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "Parameter input not ready for node " + std::to_string(node.id));
    }
    const Node& upstream_node = graph.node(input.from_node_id);
    if (!upstream_node.cached_output_high_precision) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "Parameter input not cached for node " + std::to_string(node.id));
    }
    const auto& upstream = *upstream_node.cached_output_high_precision;
    const auto value = upstream.data.find(input.from_output_name);
    if (value == upstream.data.end()) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(input.from_node_id) +
                           " missing output '" + input.from_output_name + "'");
    }
    effective.insert_or_assign(input.to_parameter_name, value->second);
  }
  return effective;
}

std::vector<PixelSize> cached_image_input_extents(const Node& node,
                                                  const GraphModel& graph) {
  std::vector<PixelSize> extents(node.image_inputs.size());
  for (std::size_t index = 0; index < node.image_inputs.size(); ++index) {
    const auto& input = node.image_inputs[index];
    if (input.from_node_id < 0 || !graph.has_node(input.from_node_id)) {
      continue;
    }
    const Node& upstream = graph.node(input.from_node_id);
    if (!upstream.cached_output_high_precision) {
      continue;
    }
    const ImageBuffer& image =
        upstream.cached_output_high_precision->image_buffer;
    extents[index] = PixelSize{image.width, image.height};
  }
  return extents;
}

}  // namespace ps
