#include "graph/graph_model.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/parameter_value_adapter.hpp"

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

}  // namespace

void GraphTopologyIndex::clear() {
  incoming_by_node.clear();
  outgoing_by_node.clear();
}

bool GraphTopologyIndex::empty() const {
  return incoming_by_node.empty() && outgoing_by_node.empty();
}

GraphModel::GraphModel(fs::path cache_root_dir)
    : cache_root(std::move(cache_root_dir)) {
  if (!cache_root.empty()) {
    fs::create_directories(cache_root);
  }
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

void GraphModel::reset_runtime_state() {
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
  clear_disk_cache_load_result();
  total_io_time_ms.store(0.0, std::memory_order_relaxed);
  skip_save_cache_.store(false, std::memory_order_relaxed);
}

void GraphModel::clear() {
  nodes_.clear();
  topology_.clear();
  reset_runtime_state();
  ++topology_generation_;
  quiet_ = true;
}

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
  nodes_ = std::move(candidate);
  rebuild_topology_index();
}

void GraphModel::replace_node(const Node& node) {
  if (!has_node(node.id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Node " + std::to_string(node.id) + " not in graph.");
  }
  NodeMap candidate = nodes_;
  candidate[node.id] = node;
  validate_node_map(candidate);
  nodes_ = std::move(candidate);
  rebuild_topology_index();
}

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
  nodes_ = std::move(candidate);
  rebuild_topology_index();
}

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
  nodes_ = std::move(candidate);
  rebuild_topology_index();
}

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
  nodes_ = std::move(candidate);
  rebuild_topology_index();
}

void GraphModel::replace_nodes(NodeMap nodes) {
  validate_node_map(nodes);
  GraphTopologyIndex topology = build_topology_index(nodes);
  nodes_ = std::move(nodes);
  topology_ = std::move(topology);
  reset_runtime_state();
  ++topology_generation_;
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

void GraphModel::rebuild_topology_index() {
  validate_node_map(nodes_);
  topology_ = build_topology_index(nodes_);
  ++topology_generation_;
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
  YAML::Node effective = node.parameters ? YAML::Clone(node.parameters)
                                         : YAML::Node(YAML::NodeType::Map);
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
    effective[input.to_parameter_name] = YAML::Clone(value->second);
  }
  return core::parameter_map_from_yaml(effective);
}

std::vector<cv::Size> cached_image_input_extents(const Node& node,
                                                 const GraphModel& graph) {
  std::vector<cv::Size> extents(node.image_inputs.size());
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
    extents[index] = cv::Size(image.width, image.height);
  }
  return extents;
}

}  // namespace ps
